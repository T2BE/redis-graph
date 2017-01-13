#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include "graph/edge.h"
#include "graph/node.h"
#include "graph/graph.h"

#include "value.h"
#include "triplet.h"
#include "value_cmp.h"
#include "redismodule.h"
#include "query_executor.h"
#include "resultset.h"

#include "rmutil/util.h"
#include "rmutil/vector.h"
#include "rmutil/test_util.h"

#include "parser/ast.h"
#include "parser/grammar.h"
#include "parser/parser_common.h"

#include "aggregate/functions.h"
#include "grouping/group.h"

#define SCORE 0.0

// Create all 6 triplets from given subject, predicate and object.
// Returns an array of triplets, caller is responsible for freeing each triplet.
RedisModuleString **hexastoreTriplets(RedisModuleCtx *ctx, const RedisModuleString *subject, const RedisModuleString *predicate, const RedisModuleString *object) {
    RedisModuleString** triplets = RedisModule_Alloc(sizeof(RedisModuleString*) * 6);

    size_t sLen = 0;
    size_t oLen = 0;
    size_t pLen = 0;

    const char* s = RedisModule_StringPtrLen(subject, &sLen);
    const char* p = RedisModule_StringPtrLen(predicate, &pLen);
    const char* o = RedisModule_StringPtrLen(object, &oLen);
    
    size_t bufLen = 6 + sLen + pLen + oLen;

    Triplet *triplet = NewTriplet(s, p, o);
    char** permutations = GetTripletPermutations(triplet);
    
    for(int i = 0; i < 6; i++) {
        RedisModuleString *permutation = RedisModule_CreateString(ctx, permutations[i], bufLen);
        triplets[i] = permutation;
        free(permutations[i]);
    }
    
    free(permutations);
    FreeTriplet(triplet);

    return triplets;
}

// Adds a new edge to the graph.
// Args:
// argv[1] graph name
// argv[2] subject
// argv[3] edge, predicate
// argv[4] object
// connect subject to object with a bi directional edge.
// Assuming both subject and object exists.
int MGraph_AddEdge(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if(argc != 5) {
        return RedisModule_WrongArity(ctx);
    }
    
    RedisModuleString *graph;
    RedisModuleString *subject;
    RedisModuleString *predicate;
    RedisModuleString *object;

    RMUtil_ParseArgs(argv, argc, 1, "ssss", &graph, &subject, &predicate, &object);

    RedisModuleKey *key = RedisModule_OpenKey(ctx, graph, REDISMODULE_WRITE);
    int keytype = RedisModule_KeyType(key);
    
    // Expecting key to be of type empty or sorted set.
    if(keytype != REDISMODULE_KEYTYPE_ZSET && keytype != REDISMODULE_KEYTYPE_EMPTY) {
        return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
    }
    
    // Create all 6 hexastore variations
    // SPO, SOP, PSO, POS, OSP, OPS
    RedisModuleString **triplets = hexastoreTriplets(ctx, subject, predicate, object);
    for(int i = 0; i < 6; i++) {
        RedisModuleString *triplet = triplets[i];
        RedisModule_ZsetAdd(key, SCORE, triplet, NULL);
        RedisModule_FreeString(ctx, triplet);
    }

    // Clean up
    RedisModule_Free(triplets);
    size_t newlen = RedisModule_ValueLength(key);
    RedisModule_CloseKey(key);
    RedisModule_ReplyWithLongLong(ctx, newlen);
    
    return REDISMODULE_OK;
}

// Removes edge from the graph.
// Args:
// argv[1] graph name
// argv[2] subject
// argv[3] edge, predicate
// argv[4] object
// removes all 6 triplets representing
// the connection (predicate) between subject and object
int MGraph_RemoveEdge(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if(argc != 5) {
        return RedisModule_WrongArity(ctx);
    }

    RedisModuleString *graph;
    RedisModuleString *subject;
    RedisModuleString *predicate;
    RedisModuleString *object;

    RMUtil_ParseArgs(argv, argc, 1, "ssss", &graph, &subject, &predicate, &object);

    size_t reply = 0;
    RedisModuleKey *key = RedisModule_OpenKey(ctx, graph, REDISMODULE_WRITE);
    int keytype = RedisModule_KeyType(key);

    // Expecting key a sorted set.
    if(keytype == REDISMODULE_KEYTYPE_ZSET) {
        // Create all 6 hexastore variations
        // SPO, SOP, PSO, POS, OSP, OPS
        RedisModuleString **triplets = hexastoreTriplets(ctx, subject, predicate, object);
        for(int i = 0; i < 6; i++) {
            RedisModuleString *triplet = triplets[i];
            int deleted;
            RedisModule_ZsetRem(key, triplet, &deleted);
            reply += deleted;
            RedisModule_FreeString(ctx, triplet);
        }
        RedisModule_Free(triplets);
    }

    // Clean up
    RedisModule_CloseKey(key);
    RedisModule_ReplyWithLongLong(ctx, reply);

    return REDISMODULE_OK;
}

// Removes given graph.
// Args:
// argv[1] graph name
// sorted set holding graph name is deleted
int MGraph_DeleteGraph(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if(argc != 2) {
        return RedisModule_WrongArity(ctx);
    }

    RedisModuleString *graph;
    RMUtil_ParseArgs(argv, argc, 1, "s", &graph);

    RedisModuleKey *key = RedisModule_OpenKey(ctx, graph, REDISMODULE_WRITE);
    int keytype = RedisModule_KeyType(key);
    size_t reply = 0;

    // Expecting key to be a sorted set.
    if(keytype == REDISMODULE_KEYTYPE_ZSET) {
        RedisModule_DeleteKey(key);
        reply = 1;
    }

    RedisModule_CloseKey(key);
    RedisModule_ReplyWithLongLong(ctx, reply);

    return REDISMODULE_OK;
}

TripletCursor* queryTriplet(RedisModuleCtx *ctx, RedisModuleString* graph, const Triplet* triplet) {
    RedisModule_AutoMemory(ctx);

    char* tripletStr = TripletToString(triplet);

    size_t bufLen = strlen(tripletStr) + 3;
    char* buf = (char*)malloc(bufLen);

    // [spo:antirez:is-friend-of: [spo:antirez:is-friend-of:\xff
    // min [spo:antirez:is-friend-of:
    // max [spo:antirez:is-friend-of:\xff

    // min
    sprintf(buf, "[%s", tripletStr);
    RedisModuleString *min = RedisModule_CreateString(ctx, buf, strlen(buf));

    // max
    sprintf(buf, "[%s\xff", tripletStr);
    RedisModuleString *max = RedisModule_CreateString(ctx, buf, bufLen);

    free(tripletStr);
    free(buf);

    RedisModuleKey *key = RedisModule_OpenKey(ctx, graph, REDISMODULE_READ);

    if(RedisModule_ZsetFirstInLexRange(key, min, max) == REDISMODULE_ERR) {
        return NULL;
    }

    return NewTripletCursor(ctx, key);
}

// Concats rmStrings using given delimiter.
// rmStrings is a vector of RedisModuleStrings pointers.
char* _concatRMStrings(const Vector* rmStrings, const char* delimiter, char** concat) {
    size_t length = 0;

    // Compute length
    for(int i = 0; i < Vector_Size(rmStrings); i++) {
        RedisModuleString* string;
        Vector_Get(rmStrings, i, &string);

        size_t len;
        RedisModule_StringPtrLen(string, &len);
        length += len;
    }

    length += strlen(delimiter) * Vector_Size(rmStrings) + 1;
    *concat = calloc(length, sizeof(char));

    for(int i = 0; i < Vector_Size(rmStrings); i++) {
        RedisModuleString* rmString;
        Vector_Get(rmStrings, i, &rmString);

        const char* string = RedisModule_StringPtrLen(rmString, NULL);

        strcat(*concat, string);
        strcat(*concat, delimiter);
    }

    // Discard last delimiter.
    (*concat)[strlen(*concat) - strlen(delimiter)] = NULL;
}

void _aggregateRecord(RedisModuleCtx *ctx, const ReturnNode* returnTree, const Graph* g) {
    // Get group
    Vector* groupKeys = ReturnClause_RetriveGroupKeys(ctx, returnTree, g);
    char* groupKey = NULL;
    _concatRMStrings(groupKeys, ",", &groupKey);
    // TODO: free groupKeys

    Group* group = NULL;
    CacheGroupGet(groupKey, &group);

    if(group == NULL) {
        // Create a new group
        // Get aggregation functions
        group = NewGroup(groupKeys, ReturnClause_GetAggFuncs(ctx, returnTree));
        CacheGroupAdd(groupKey, group);

        // TODO: no need to get inserted group
        CacheGroupGet(groupKey, &group);
    }

    // TODO: why can't we free groupKey?
    // free(groupKey);

    Vector* valsToAgg = ReturnClause_RetriveGroupAggVals(ctx, returnTree, g);

    // Run each value through its coresponding function.
    for(int i = 0; i < Vector_Size(valsToAgg); i++) {
        RedisModuleString* value = NULL;
        Vector_Get(valsToAgg, i, &value);
        size_t len;
        const char* strValue = RedisModule_StringPtrLen(value, &len);

        // Convert to double SIValue.
        SIValue numValue;
        numValue.type = T_DOUBLE;
        SI_ParseValue(&numValue, strValue, len);

        AggCtx* funcCtx = NULL;
        Vector_Get(group->aggregationFunctions, i, &funcCtx);
        Agg_Step(funcCtx, &numValue, 1);
    }

    // TODO: free valsToAgg
}

void QueryNode(RedisModuleCtx *ctx, RedisModuleString *graphName, Graph* g, Node* n, Vector* entryPoints, const QE_FilterNode* filterTree, const ReturnNode* returnTree, ResultSet* returnedSet) {
    Node* src = n;
    for(int i = 0; i < Vector_Size(src->outgoingEdges) && !ResultSet_Full(returnedSet); i++) {

        Edge* edge;
        Vector_Get(src->outgoingEdges, i, &edge);
        Node* dest = edge->dest;
        
        // Create a triplet out of edge
        Triplet* triplet = TripletFromEdge(edge);
        
        // Query graph using triplet, no filters are applied at this stage
        TripletCursor* cursor = queryTriplet(ctx, graphName, triplet);
        FreeTriplet(triplet);

        // Backup original node IDs
        char* srcOriginalID = src->id;
        char* edgeOriginalID = edge->relationship;
        char* destOriginalID = dest->id;

        // Run through cursor.
        Triplet* result;
        while(!ResultSet_Full(returnedSet) && (result = TripletCursorNext(cursor), result != NULL)) {
            // Copy result values to graph, override node IDs.
            src->id = result->subject;
            edge->relationship = result->predicate;
            dest->id = result->object;

            // Advance to next node
            if(Vector_Size(dest->outgoingEdges) > 0) {
                QueryNode(ctx, graphName, g, dest, entryPoints, filterTree, returnTree, returnedSet);
            } else if(Vector_Size(entryPoints) > 0) {
                // Additional entry point
                Node* entryPoint = NULL;
                Vector_Pop(entryPoints, &entryPoint);
                QueryNode(ctx, graphName, g, entryPoint, entryPoints, filterTree, returnTree, returnedSet);
                // Restore state, for next iteration.
                Vector_Push(entryPoints, entryPoint);
            } else {
                // We've reach the end of the graph
                // Pass through filter, apply filters specified in where clause.
                if(filterTree == NULL || applyFilters(ctx, g, filterTree)) {
                    if(returnedSet->aggregated) {
                        _aggregateRecord(ctx, returnTree, g);
                    } else {
                        // Append to final result set.
                        ResultSet_AddRecord(returnedSet, ReturnClause_RetrivePropValues(ctx, returnTree, g));
                    }
                } // End of filtering
            }
        } // End of cursor loop
        FreeTripletCursor(cursor);
        
        // Restore nodes original IDs.
        src->id = srcOriginalID;
        edge->relationship = edgeOriginalID;
        dest->id = destOriginalID;
    }
}

// Queries graph
// Args:
// argv[1] graph name
// argv[2] query to execute
int MGraph_Query(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 2) return RedisModule_WrongArity(ctx);

    // Time query execution
    clock_t start = clock();
    clock_t end;

    RedisModuleString *graphName;
    RedisModuleString *query;
    RMUtil_ParseArgs(argv, argc, 1, "ss", &graphName, &query);
    
    size_t qLen;
    const char* q = RedisModule_StringPtrLen(query, &qLen);

    // Parse query, get AST.
    QueryExpressionNode* parseTree = ParseQuery(q, qLen);
    Graph* graph = BuildGraph(parseTree->matchNode);

    QE_FilterNode* filterTree = NULL;
    if(parseTree->whereNode != NULL) {
        filterTree = BuildFiltersTree(parseTree->whereNode->filters);
    }

    Vector* entryPoints = Graph_GetNDegreeNodes(graph, 0);
    Node* startNode = NULL;
    Vector_Pop(entryPoints, &startNode);

    CacheGroupClear(); // Clear group cache before query execution.
    
    // Construct a new empty result-set.
    ResultSet* resultSet = NewResultSet(parseTree);
    QueryNode(ctx, graphName, graph, startNode, entryPoints, filterTree, parseTree->returnNode, resultSet);

    // Send result-set back to client.
    ResultSet_Replay(ctx, resultSet);

    // Report execution timing
    end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
    double elapsedMS = elapsed * 1000; 
    char* strElapsed = (char*)malloc(sizeof(char) * strlen("Query internal execution time: miliseconds") + 8);
    sprintf(strElapsed, "Query internal execution time: %f miliseconds", elapsedMS);
    RedisModule_ReplyWithStringBuffer(ctx, strElapsed, strlen(strElapsed));

    // TODO: free memory.
    // free(strElapsed);
    // Free AST
    // FreeQueryExpressionNode(parseTree);
    ResultSet_Free(ctx, resultSet);

    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    Agg_RegisterFuncs();

    if (RedisModule_Init(ctx, "graph", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    RMUtil_RegisterWriteCmd(ctx, "graph.ADDEDGE", MGraph_AddEdge);
    RMUtil_RegisterWriteCmd(ctx, "graph.REMOVEEDGE", MGraph_RemoveEdge);
    RMUtil_RegisterWriteCmd(ctx, "graph.DELETE", MGraph_DeleteGraph);
    RMUtil_RegisterWriteCmd(ctx, "graph.QUERY", MGraph_Query);

    return REDISMODULE_OK;
}