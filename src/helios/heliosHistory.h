/**
 * @file heliosHistory.h
 *
 * @brief Header file for heliosHistory.c
 *
 * @date 05/26/2026
 * @author Ved Lakshminarayanan <vlakshmi@andrew.cmu.edu>
 */

#ifndef HELIOS_UCH_H
#define HELIOS_UCH_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define UCH_LOAD_SIZE 6 
#define UCH_STORE_SIZE 1
#define CACHEBLOCK_SIZE 64 
#define CACHEBLOCK_BITS 6 

typedef struct {
    uint64_t pc;
    uint64_t cacheBlockAddress;
    uint64_t virtualAddress;
    uint64_t commitNumber;
    uint8_t accessSize;
    bool valid;
} LoadUCHEntry;

typedef struct {
    uint64_t pc;
    uint64_t cacheBlockAddress;
    uint64_t virtualAddress;
    uint64_t commitNumber;
    uint8_t accessSize;
    bool valid;
} StoreUCHEntry;

extern LoadUCHEntry loadUCH[UCH_LOAD_SIZE];
extern StoreUCHEntry storeUCH[UCH_STORE_SIZE];
extern int loadUCHHead;
extern int storeUCHHead;

void initLSUCH(void);
void UCHAddLoad(uint64_t pc, uint64_t virtualAddress, uint8_t accessSize, uint64_t commitNumber);
void UCHAddStore(uint64_t pc, uint64_t virtualAddress, uint8_t accessSize, uint64_t commitNumber);
bool canFuse(uint64_t address1, uint64_t address2, uint8_t size1, uint8_t size2);
bool UCHSearchLoadFusion(uint64_t virtualAddress, uint8_t accessSize, uint64_t commitNumber, 
                         uint64_t *headPC, uint16_t *distanceToHead);
bool UCHSearchStoreFusion(uint64_t virtualAddress, uint8_t accessSize, uint64_t commitNumber, 
                          uint64_t *headPC, uint16_t *distanceToHead);
void markLoadFused(uint64_t commitNumber);
void markStoreFused(uint64_t commitNumber);
uint16_t updateLoadUCH(uint64_t pc, uint64_t virtualAddress, uint8_t accessSize, uint64_t commitNumber, 
                       uint64_t *headPC);
uint16_t updateStoreUCH(uint64_t pc, uint64_t virtualAddress, uint8_t accessSize, uint64_t commitNumber, 
                        uint64_t *headPC);
void resetUCH(uint64_t commitNumber);

static inline bool isContiguous(uint64_t address1, uint64_t address2, uint8_t size1, uint8_t size2) {
    if (address1 == address2) {
        return true;
    }
    if (address1 < address2) {
        return address2 - address1 <= size1;
    }
    if (address1 > address2) {
        return address1 - address2 <= size2;
    }
    return false;
}

static inline bool isSameCacheBlock(uint64_t address1, uint64_t address2, uint8_t size1, uint8_t size2) {
    uint64_t address1End = address1 + size1 - 1;
    uint64_t address2End = address2 + size2 - 1;

    uint64_t block1 = address1 & ~((1ULL << CACHEBLOCK_BITS) - 1);
    uint64_t block2 = address2 & ~((1ULL << CACHEBLOCK_BITS) - 1);
    uint64_t end1 = address1End & ~((1ULL << CACHEBLOCK_BITS) - 1);
    uint64_t end2 = address2End & ~((1ULL << CACHEBLOCK_BITS) - 1);

    return ((block1 == block2) && (block1 == end1) && (block2 == end2)); 
}

static inline bool isNextBlock(uint64_t address1, uint64_t address2, uint8_t size1, uint8_t size2) {
    uint64_t address1End = address1 + size1 - 1;
    uint64_t address2End = address2 + size2 - 1;

    if ((address1 <= address2 && (address2End - address1 < CACHEBLOCK_SIZE)) ||     
        (address2 <= address1 && (address1End - address2 < CACHEBLOCK_SIZE))) {
        return true;
    }
    else {
        return false;
    }
}

static inline uint64_t getCacheBlockAddress(uint64_t virtualAddress) {
    return virtualAddress & ~((1ULL << CACHEBLOCK_BITS) - 1);
}

#endif