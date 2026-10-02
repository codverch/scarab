/**
 * @file heliosFusion.h
 *
 * @brief Header file for heliosFusion.c
 *
 * @date 05/28/2026
 * @author Ved Lakshminarayanan <vlakshmi@andrew.cmu.edu>
 */

#ifndef HELIOS_FUSION_H
#define HELIOS_FUSION_H
#include <stdbool.h>
#include <stdint.h>

#include "heliosHistory.h"
#include "heliosFusionPredictor.h"
#include "op.h"

#define MAX_FUSION_REGS 4

typedef struct {
    uint64_t globalMicroOpNumber;
    uint64_t cacheBlockAddress;
    uint64_t memoryAddress;
    uint8_t accessSize;
    bool valid;
    bool headConsumed;
    bool fused;
    Op* headOp;
} LoadHeadTableEntry;

#define LOAD_HEAD_TABLE_SIZE 140
extern LoadHeadTableEntry loadHeadTable[LOAD_HEAD_TABLE_SIZE];

typedef struct {
    uint64_t globalMicroOpNumber;
    uint64_t cacheBlockAddress;
    uint64_t memoryAddress;
    uint8_t accessSize;
    bool valid;
    bool headConsumed;
    bool fused;
    Op* headOp;
} StoreHeadTableEntry;

#define STORE_HEAD_TABLE_SIZE 140
extern StoreHeadTableEntry storeHeadTable[STORE_HEAD_TABLE_SIZE];

typedef struct {
    uint64_t globalMicroOpNumber;
    uint8_t numDestRegs;
    uint8_t numSrcRegs;
    uint16_t destRegs[MAX_FUSION_REGS];
    uint16_t srcRegs[MAX_FUSION_REGS];
    bool valid;
    bool isSerializing;
} RegTrackEntry;

/* Every in-flight store, in fetch order, whether or not it is a head candidate. Used to unfuse a
 * store pair in place when a catalyst store writes the same cache block as the pair. */
typedef struct {
    uint64_t globalMicroOpNumber;
    uint64_t memoryAddress;
    uint8_t accessSize;
    bool valid;
} StoreTrackEntry;

#define STORE_TRACK_TABLE_SIZE 256
extern StoreTrackEntry storeTrackTable[STORE_TRACK_TABLE_SIZE];
extern int storeTrackTableHead;

#define REG_TRACK_TABLE_SIZE 1024
extern RegTrackEntry regTrackTable[REG_TRACK_TABLE_SIZE];
extern int regTrackTableHead;

#define MAX_NCSF_NEST 2
#define NCSF_RING_SIZE 8

typedef struct {
    uint64_t headMicroOpNumber;
    uint64_t tailMicroOpNumber;
    bool valid;
} FusionIntervalEntry;

extern FusionIntervalEntry activeFusions[NCSF_RING_SIZE];
extern int activeFusionsHead;

#ifdef __cplusplus
extern "C" {
#endif

void heliosFusionInit(void);
void heliosFetchHook(Op *op);
void heliosCommit(Op *op);
void addLoadHead(Op *op);
void addStoreHead(Op *op);
void trackStore(Op *op);
void trackRegWrites(Op *op);
bool checkDeadlock(Op *tailOp, uint64_t headMicroOpNumber);
void validatePrediction(Op *op);
void flushTables(uint64_t globalMicroOpNumber);
void checkFusionCandidates(Op *op);
void heliosAddFusionDep(Op *op);
void heliosCompleteFusedTail(Op *head);
void heliosWakeFusedStoreTailDeps(Op *head);
Flag heliosExtCommitGroupBlocks(Op *head);
void heliosUnblockRobHead(Op *op);

#ifdef __cplusplus
}
#endif

#endif