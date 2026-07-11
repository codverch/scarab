/**
 * @file heliosHistory.c
 *
 * @brief Provides functionality to manipulate the load and store UCH data structures.
 *
 * @date 05/26/2026
 * @author Ved Lakshminarayanan <vlakshmi@andrew.cmu.edu>
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "globals/global_types.h"
#include "general.param.h"
#include "heliosHistory.h"

LoadUCHEntry loadUCH[UCH_LOAD_SIZE];
int loadUCHHead = 0;
StoreUCHEntry storeUCH[UCH_STORE_SIZE];
int storeUCHHead = 0;

/**
 * @brief Initialize the load and store UCH data structures.
 * 
 * @param void 
 * 
 * @return void
 */
void initLSUCH(void) {

    // Initialize the load UCH. 
    memset(loadUCH, 0, sizeof(loadUCH));
    loadUCHHead = 0;
    for (int i = 0; i < UCH_LOAD_SIZE; i++) {
        loadUCH[i].valid = false;
        loadUCH[i].commitNumber = 0;
        loadUCH[i].pc = 0;
        loadUCH[i].cacheBlockAddress = 0;
        loadUCH[i].virtualAddress = 0;
        loadUCH[i].accessSize = 0;
    }

    // Initialize the store UCH. Should only have one entry.
    memset(storeUCH, 0, sizeof(storeUCH));
    storeUCHHead = 0;
    for (int i = 0; i < UCH_STORE_SIZE; i++) {
        storeUCH[i].pc = 0;
        storeUCH[i].cacheBlockAddress = 0;
        storeUCH[i].virtualAddress = 0;
        storeUCH[i].commitNumber = 0;
        storeUCH[i].accessSize = 0;
        storeUCH[i].valid = false;
    }
}

/**
 * @brief Find an invalid/outdated entry to eject from the UCH. Function is
 * not called when updating the store UCH, as the store UCH only has one entry.
 * 
 * @param void 
 * 
 * @return Index of the victim to eject.
 */
static int findVictimToEject(void) {
    int invalidIndexToEject = -1;
    int oldestIndexToEject = -1;
    uint64_t oldestCommitNumber = UINT64_MAX;

    // Loop through the load UCH and find an invalid entry to eject. 
    // If no invalid entry is found, find the oldest entry to eject. 
    for (int i = 0; i < UCH_LOAD_SIZE; i++) {
        if (!loadUCH[i].valid) {
            if (invalidIndexToEject == -1) {
                invalidIndexToEject = i;
            }
        }

        else if (loadUCH[i].commitNumber < oldestCommitNumber) {
            oldestCommitNumber = loadUCH[i].commitNumber;
            oldestIndexToEject = i;
        }
    }

    if (invalidIndexToEject != -1) {
        return invalidIndexToEject;
    }
    else {
        return oldestIndexToEject;
    }
}

/**
 * @brief Verifies whether two loads/stores can be fused based on the
 * locations in memory they access. Contiguous memory accesses, hits to
 * the same cache block, or hits to adjacent cache blocks are considered
 * fusable.
 * 
 * @param address1 Address accessed by the first memory op.
 * @param address2 Address accessed by the second memory op.
 * @param size1 Size of the first memory op access.
 * @param size2 Size of the second memory op access.
 * 
 * @return Boolean indicating if the memory ops can be fused.
 */
bool canFuse(uint64_t address1, uint64_t address2, uint8_t size1, uint8_t size2) {
    if (isContiguous(address1, address2, size1, size2)) {
        return true;
    }
    else if (isSameCacheBlock(address1, address2, size1, size2)) {
        return true;
    }
    else if (isNextBlock(address1, address2, size1, size2)) {
        return true;
    }
    else {
        return false;
    }
}

/**
 * @brief Finds a victim to eject from the load UCH and adds the new load to the UCH.
 * 
 * @param pc New load's PC.
 * @param virtualAddress New load's virtual address. 
 * @param accessSize New load's access size.
 * @param CommitNumber New load's commit number. 
 * 
 * @return void
 */
void UCHAddLoad(uint64_t pc, uint64_t virtualAddress, uint8_t accessSize, uint64_t commitNumber) {
    int indexToUpdate = findVictimToEject();

    LoadUCHEntry *entry = &loadUCH[indexToUpdate];

    entry->pc = pc;
    entry->cacheBlockAddress = getCacheBlockAddress(virtualAddress);
    entry->virtualAddress = virtualAddress;
    entry->accessSize = accessSize;
    entry->commitNumber = commitNumber;
    entry->valid = true;
}

/**
 * @brief Updates the store UCH with a new store. Only one entry exists in the store UCH.
 * 
 * @param pc New store's PC.
 * @param virtualAddress New stores's virtual address. 
 * @param accessSize New stores's access size.
 * @param CommitNumber New stores's commit number. 
 * 
 * @return void
 */
void UCHAddStore(uint64_t pc, uint64_t virtualAddress, uint8_t accessSize, uint64_t commitNumber) {
    StoreUCHEntry *entry = &storeUCH[storeUCHHead];

    entry->pc = pc;
    entry->cacheBlockAddress = getCacheBlockAddress(virtualAddress);
    entry->virtualAddress = virtualAddress;
    entry->accessSize = accessSize;
    entry->commitNumber = commitNumber;
    entry->valid = true;
}

/**
 * @brief Search the load UCH for a possible, fusion candidate. If a fusion candidate is found,
 * mark the load as fused and return the distance to the head. Otherwise, add the load to
 * the UCH as a possible head to fuse with.
 * 
 * @param pc Load's PC.
 * @param virtualAddress Load's virtual address. 
 * @param accessSize Load's access size.
 * @param CommitNumber Load's commit number. 
 * @param headPC Pointer to the fusion pair's head PC. 
 * 
 * @return Distance to the head of the fusion pair. 0 if no fusion candidate is found.
 */
uint16_t updateLoadUCH(uint64_t pc, uint64_t virtualAddress, uint8_t accessSize, uint64_t commitNumber, uint64_t *headPC) {
    uint16_t distanceToHead = 0;
    uint64_t foundHeadPC = 0;

    // Search the UCH for a fusion candidate. foundFusionCandidate will be true if a fusion candidate is found.
    bool foundFusionCandidate = UCHSearchLoadFusion(virtualAddress, accessSize, commitNumber, 
                                                    &foundHeadPC, &distanceToHead);
    
    if (headPC != NULL) {
        *headPC = foundHeadPC;
    }
    
    if (foundFusionCandidate) {
        uint64_t headCommitNumber = commitNumber - (uint64_t)distanceToHead;
        markLoadFused(headCommitNumber);
        return distanceToHead;
    }
    else {
        UCHAddLoad(pc, virtualAddress, accessSize, commitNumber);
        return 0;
    }
}

/**
 * @brief Search the store UCH for a possible, fusion candidate. If a fusion candidate is found,
 * mark the store as fused and return the distance to the head. Otherwise, add the store to
 * the UCH as a possible head to fuse with.
 * 
 * @param pc Stores's PC.
 * @param virtualAddress Stores's virtual address. 
 * @param accessSize Stores's access size.
 * @param CommitNumber Stores's commit number. 
 * @param headPC Pointer to the fusion pair's head PC. 
 * 
 * @return Distance to the head of the fusion pair. 0 if no fusion candidate is found.
 */
uint16_t updateStoreUCH(uint64_t pc, uint64_t virtualAddress, uint8_t accessSize, uint64_t commitNumber, uint64_t *headPC) {
    uint16_t distanceToHead = 0;
    uint64_t foundHeadPC = 0;

    // Search the UCH for a fusion candidate. foundFusionCandidate will be true if a fusion candidate is found.
    bool foundFusionCandidate = UCHSearchStoreFusion(virtualAddress, accessSize, commitNumber, 
                                                     &foundHeadPC, &distanceToHead);
    
    if (headPC != NULL) {
        *headPC = foundHeadPC;
    }
    
    if (foundFusionCandidate) {
        uint64_t headCommitNumber = commitNumber - (uint64_t)distanceToHead;
        markStoreFused(headCommitNumber);
        return distanceToHead;
    }
    else {
        UCHAddStore(pc, virtualAddress, accessSize, commitNumber);
        return 0;
    }
}

/**
 * @brief Search the load UCH for a possible, fusion candidate. Returns true if a fusion candidate is found,
 * false otherwise. If a fusion candidate is found, the pointer arguments are updated accordingly. This function
 * grabs the most recent, fusable load.
 * 
 * @param virtualAddress Load's virtual address. 
 * @param accessSize Load's access size.
 * @param CommitNumber Load's commit number. 
 * @param headPC Pointer to the fusion pair's head PC.
 * @param distanceToHead Pointer to the distance to the head of the fusion pair.
 * 
 * @return Boolean indicating if a fusion candidate is found.
 */
bool UCHSearchLoadFusion(uint64_t virtualAddress, uint8_t accessSize, uint64_t commitNumber, uint64_t *headPC, uint16_t *distanceToHead) {
    uint64_t mostRecentFusableCommitNumber = 0;
    bool fusionCandidateFound = false;
    
    // Loop through the load UCH and attempt to find a fusion candidate.
    for (int i = 0; i < UCH_LOAD_SIZE; i++) {
        LoadUCHEntry *entry = &loadUCH[i];

        if (!entry->valid || entry->accessSize == 0 || commitNumber <= entry->commitNumber) {
            continue;
        }

        // Check if the op hits the same cache block. Requirements for predictor training are slightly stricter
        // given that an entry's UCH tag is constructed from its cache-line address. Therefore, 
        // pairs are only trained if they hit the same cache block.
        bool sameCacheBlock = (getCacheBlockAddress(virtualAddress) == entry->cacheBlockAddress);

        if (!sameCacheBlock) {
            continue;
        }

        uint64_t distance = commitNumber - entry->commitNumber;

        if (distance > HELIOS_FUSION_WINDOW) {
            continue;
        }

        // Grab the most recent, fusable load.
        if (entry->commitNumber > mostRecentFusableCommitNumber) {
            mostRecentFusableCommitNumber = entry->commitNumber;
            fusionCandidateFound = true;
            if ((headPC != NULL) && (distanceToHead != NULL)) {
                *headPC = entry->pc;
                *distanceToHead = (uint16_t)distance;
            }
        }
    }
    
    // If no fusion candidate is found, return false. Otherwise, return true.
    if (!fusionCandidateFound) {
        if ((headPC != NULL) && (distanceToHead != NULL)) {
            *headPC = 0;
            *distanceToHead = 0;
        }
        return false;
    }
    else {
        return true;
    }
}

/**
 * @brief Search the store UCH for a possible, fusion candidate. Returns true if a fusion candidate is found,
 * false otherwise. If a fusion candidate is found, the pointer arguments are updated accordingly. This function
 * grabs the most recent, fusable store.
 * 
 * @param virtualAddress Stores's virtual address. 
 * @param accessSize Stores's access size.
 * @param CommitNumber Stores's commit number. 
 * @param headPC Pointer to the fusion pair's head PC.
 * @param distanceToHead Pointer to the distance to the head of the fusion pair.
 * 
 * @return Boolean indicating if a fusion candidate is found.
 */
bool UCHSearchStoreFusion(uint64_t virtualAddress, uint8_t accessSize, uint64_t commitNumber, uint64_t *headPC, uint16_t *distanceToHead) {
    
    // Grab the only entry in the store UCH.
    StoreUCHEntry *entry = &storeUCH[storeUCHHead];

    if (!entry->valid || entry->accessSize == 0 || commitNumber <= entry->commitNumber) {
        return false;
    }

    // Check if the op hits the same cache block. Requirements for predictor training are slightly stricter
    // given that an entry's UCH tag is constructed from its cache-line address. Therefore, 
    // pairs are only trained if they hit the same cache block.
    bool sameCacheBlock = (getCacheBlockAddress(virtualAddress) == entry->cacheBlockAddress);

    if (!sameCacheBlock) {
        return false;
    }

    uint64_t distance = commitNumber - entry->commitNumber;

    if (distance > HELIOS_FUSION_WINDOW) {
        return false;
    }

    // If a fusion candidate is found, update the pointer arguments and return true.
    if ((headPC != NULL) && (distanceToHead != NULL)) {
        *headPC = entry->pc;
        *distanceToHead = (uint16_t)distance;
        return true;
    }

    // If no fusion candidate is found, return false.
    if (headPC != NULL) {
        *headPC = 0;
    }
    return false;
}

/**
 * @brief Marks the store in the UCH as invalid. This store is no longer viable
 * for fusion as it has already been fused.
 * 
 * @param CommitNumber Stores's commit number. 
 * 
 * @return void
 */
void markStoreFused(uint64_t commitNumber) {
    StoreUCHEntry *entry = &storeUCH[storeUCHHead];
    if (entry->valid && (entry->commitNumber == commitNumber)) {
        entry->valid = false;
    }
}

/**
 * @brief Marks the load in the UCH as invalid. This load is no longer viable
 * for fusion as it has already been fused.
 * 
 * @param CommitNumber Load's commit number. 
 * 
 * @return void
 */
void markLoadFused(uint64_t commitNumber) {

    // Loop through the load UCH and mark the load as invalid.
    for (int i = 0; i < UCH_LOAD_SIZE; i++) {
        LoadUCHEntry *entry = &loadUCH[i];
        if (entry->valid && (entry->commitNumber == commitNumber)) {
            entry->valid = false;
        }
    }
}

/**
 * @brief Function used to invalidate UCH entries that were mistakingly committed to the UCH
 * due to an error upstream. 
 * 
 * @param CommitNumber Commit number used to invalidate the prior UCH entries.
 * 
 * @return void
 */
void resetUCH(uint64_t commitNumber) {

    // Loop through the load UCH and mark relevant entries as invalid.
    for (int i = 0; i < UCH_LOAD_SIZE; i++) {
        LoadUCHEntry *entry = &loadUCH[i];
        if ((entry->commitNumber < commitNumber) && entry->valid) {
            entry->valid = false;
        }
    }

    // Check the store UCH and mark the entry as invalid if necessary. 
    StoreUCHEntry *entry = &storeUCH[storeUCHHead];
    if ((entry->commitNumber < commitNumber) && entry->valid) {
        entry->valid = false;
    }
}