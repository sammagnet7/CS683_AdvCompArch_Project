/*************************************************************************************************************************
Authors:
Samuel Pakalapati - samuelpakalapati@gmail.com
Biswabandan Panda - biswap@cse.iitk.ac.in
Nilay Shah - nilays@iitk.ac.in
Neelu Shivprakash kalani - neeluk@cse.iitk.ac.in
**************************************************************************************************************************/

/*************************************************************************************************************************
Source code of "Bouquet of Instruction Pointers: Instruction Pointer Classifier-based Spatial Hardware Prefetching"
appeared (to appear) in ISCA 2020: https://www.iscaconf.org/isca2020/program/. The paper is available at
https://www.cse.iitk.ac.in/users/biswap/IPCP_ISCA20.pdf. The source code can be used with the ChampSim simulator
https://github.com/ChampSim . Note that the authors have used a modified ChampSim that supports detailed virtual
memory sub-system. Performance numbers may increase/decrease marginally
based on the virtual memory-subsystem support. Also for PIPT L1-D caches, this code may demand 1 to 1.5KB additional
storage for various hardware tables.
**************************************************************************************************************************/

#include "ooo_cpu.h"
#include "cache.h"
#include <vector>
#include <stdio.h>
#include <map>
#include <set>
#include <cassert>
#include "training_unit.h"
#include "off_chip_info.h"

/**************************************************************************************************************************
Note that variables uint64_t pref_useful[NUM_CPUS][6], pref_filled[NUM_CPUS][6], pref_late[NUM_CPUS][6]; are to be declared
as members of the CACHE class in inc/cache.h and modified in src/cache.cc where the second level index denotes the IPCP prefetch
class type for each variable which can be extracted through pf_metadata. A prefetch is considered in pref_useful when a cache
blocks gets a hit and its prefetch bit is set. Whenever a cache block is filled (in handle_fill) and its type is prefetch,
pref_fill is incremented. The pref_late variable is modified whenever a demand request merges with a prefetch request or
vice versa in the cache's MSHR as, if the prefetch would've been on time, the demand request would've hit in the cache.
****************************************************************************************************************************/

#define DO_PREF
#define NUM_BLOOM_ENTRIES 4096 // For book-keeping purposes
#define NUM_IP_TABLE_L1_ENTRIES 64
#define NUM_CSPT_ENTRIES 128 // = 2^NUM_SIG_BITS
#define NUM_SIG_BITS 7       // num of bits in signature
#define NUM_IP_INDEX_BITS 6
#define NUM_IP_TAG_BITS 9
#define NUM_PAGE_TAG_BITS 2
#define S_TYPE 1    // stream
#define CS_TYPE 2   // constant stride
#define CPLX_TYPE 3 // complex stride
#define NL_TYPE 4   // next line
#define IA_TYPE 5   // Irregular access
#define CPLX_DIST 0
#define NUM_CLASSES 6
#define NUM_OF_RR_ENTRIES 32      // recent request filter entries
#define RR_TAG_MASK 0xFFF         // 12 bits of prefetch line address are stored in recent request filter
#define NUM_RST_ENTRIES 8         // region stream table entries
#define MAX_POS_NEG_COUNT 64      // 6-bit saturating counter
#define NUM_OF_LINES_IN_REGION 32 // 32 cache lines in 2KB region
#define REGION_OFFSET_MASK 0x1F   // 5-bit offset for 2KB region
                                  // # -> No. of times
unsigned int total_access;        // # access to the prefetcher
unsigned int predictions;         // # predictions for prefetch made by ISB
unsigned int no_prediction;       // # address with no prior structural address
unsigned int stream_end;          // # prefetch candidate address is at stream boundary
unsigned int no_translation;      // # given structural address has no physical address mapping

uint64_t last_address;            // last address seen by ISB

#define BUFFER_SIZE 128
// ------------------------------------------ISB Structures--------------------------------------
struct PrefetchBuffer
{
    uint64_t buffer[BUFFER_SIZE];
    bool valid[BUFFER_SIZE];
    unsigned int next_index;

    void reset()
    {
        for (unsigned int i = 0; i < BUFFER_SIZE; i++)
            valid[i] = false;
        next_index = 0;
    }
    void add(uint64_t addr)
    {
        buffer[next_index] = addr;
        valid[next_index] = true;
        next_index = (next_index + 1) % BUFFER_SIZE;
    }

    void issue(unsigned int i)
    {
        assert(valid[i]);
        valid[i] = false;
    }

    bool get(unsigned int i, uint64_t &addr)
    {
        addr = buffer[i];
        return valid[i];
    }
};

struct ISB_prefetcher_t
{
    TUCache training_unit;

    OffChipInfo oci;

    uint64_t alloc_counter;
    uint64_t last_page;

    uint64_t stream_divergence_count;
    uint64_t stream_divergence_new_stream;
    uint64_t candidate_tlb_miss;
    uint64_t candidate_diff;
    PrefetchBuffer prefetch_buffer;

    unsigned int isb_train(unsigned int str_addr_A, uint64_t phy_addr_B)
    {
        // Algorithm for training correlated pair (A,B)
        // Step 2a : If SA(A)+1 does not exist, assign B SA(A)+1
        // Step 2b : If SA(A)+1 exists, copy the stream starting at S(A)+1 and then assign B SA(A)+1

        unsigned int str_addr_B;
        bool str_addr_B_exists = oci.get_structural_address(phy_addr_B, str_addr_B);
#ifdef DEBUG
        std::cout << "-----S(A) : " << str_addr_A << std::endl;
#endif
        // If S(A) is at a stream boundary return, we don't need to worry about B because it is as good as a stream start
        if ((str_addr_A + 1) % STREAM_MAX_LENGTH == 0)
        {
            if (!str_addr_B_exists)
            {
                str_addr_B = assign_structural_addr();
                oci.update(phy_addr_B, str_addr_B);
            }
            return str_addr_B;
        }

        bool invalidated = false;
        if (str_addr_B_exists)
        {
            // if(str_addr_B == str_addr_A + 1){
            if (str_addr_B % STREAM_MAX_LENGTH == (str_addr_A + 1) % STREAM_MAX_LENGTH)
            {
#ifdef DEBUG
                std::cout << phy_addr_B << " has a structural address of " << str_addr_B << " conf++ " << std::endl;
#endif
                oci.increase_confidence(phy_addr_B);
                return str_addr_B;
            }
            else
            {
#ifdef DEBUG
                std::cout << phy_addr_B << " has a structural address of " << str_addr_B << " conf-- " << std::endl;
#endif
                bool ret = oci.lower_confidence(phy_addr_B);
                if (ret)
                    return str_addr_B;
#ifdef DEBUG
                std::cout << "Invalidate " << std::endl;
#endif
                invalidated = true;
                oci.invalidate(phy_addr_B, str_addr_B);
                str_addr_B_exists = false;
            }
        }

        assert(!str_addr_B_exists);

        // Handle stream divergence

        unsigned int i = 1;
        uint64_t phy_addr_Aplus1;
        bool phy_addr_Aplus1_exists = oci.get_physical_address(phy_addr_Aplus1, str_addr_A + 1);

        if (phy_addr_Aplus1_exists)
            stream_divergence_count++;
// #define CFIX
#ifdef CFIX
        while (phy_addr_Aplus1_exists)
        {
#ifdef DEBUG
            std::cout << "-----S(A)+1 : " << phy_addr_Aplus1 << std::endl;
#endif
            i++;

            if ((str_addr_A + i) % STREAM_MAX_LENGTH == 0)
            {
                stream_divergence_new_stream++;
                str_addr_B = assign_structural_addr();
                break;
            }
            phy_addr_Aplus1_exists = oci.get_physical_address(phy_addr_Aplus1, str_addr_A + i);

            // oci.reassign_stream(str_addr_A+1, assign_structural_addr()); //TODO TBD. Should we re-assign??
        }
        if (!phy_addr_Aplus1_exists)
            str_addr_B = str_addr_A + i;

#else
        if (phy_addr_Aplus1_exists)
        {
            // Old solution: Nothing fancy, just assign a new address
            stream_divergence_count++;
            if (invalidated)
                return str_addr_B;
            else
                str_addr_B = assign_structural_addr();
        }
        else
            str_addr_B = str_addr_A + 1;

#endif

#ifdef DEBUG
        std::cout << (void *)phy_addr_B << " allotted a structural address of " << str_addr_B << std::endl;
        std::cout << "-----S(B) : " << str_addr_B << std::endl;
#endif
        oci.update(phy_addr_B, str_addr_B);

        return str_addr_B;
    }

    vector<uint64_t> isb_predict(uint64_t trigger_phy_addr, unsigned int trigger_str_addr, uint64_t ip, uint64_t degree)
    {
#ifdef DEBUG
        std::cout << "*Trigger Str addr " << trigger_str_addr << std::endl;
#endif
        uint64_t candidate_phy_addr;
        vector<uint64_t> candidates;
        candidates.clear();

#ifndef RESTRICT_REGION
        unsigned int lookahead = 1;
        unsigned int ideal = 0;
        for (unsigned int i = 0; i < STREAM_MAX_LENGTH; i++)
        {
            if (ideal >= degree)
                break;
            uint64_t str_addr_candidate = trigger_str_addr + lookahead + i;
            if (str_addr_candidate % STREAM_MAX_LENGTH == 0)
            {
                stream_end++;
                break;
            }

            bool ret = oci.get_physical_address(candidate_phy_addr, str_addr_candidate);
            if (ret)
            {
                ideal++;
                candidates.push_back(candidate_phy_addr);
            }
            else
                no_translation++;
        }
#else
        unsigned int num_prefetched = 0;
        for (unsigned int i = 0; i < STREAM_MAX_LENGTH; i++)
        {
            uint64_t str_addr_candidate = ((trigger_str_addr >> STREAM_MAX_LENGTH_BITS) << STREAM_MAX_LENGTH_BITS) + i;

            if (str_addr_candidate == trigger_str_addr)
                continue;

            bool ret = oci.get_physical_address(candidate_phy_addr, str_addr_candidate);

            if (ret)
            // if(ret && ((candidate_phy_addr >> 12) == (trigger_phy_addr >> 12)) )
            {
                candidates.push_back(candidate_phy_addr);

                if (num_prefetched >= DEGREE)
                    break;
            }
        }
#endif

        return candidates;
    }

    bool access_training_unit(uint64_t key, uint64_t &last_phy_addr, unsigned int &last_str_addr, uint64_t next_addr)
    {
        // TrainingUnitEntry* curr_training_entry = training_unit.find(key)->second;
        bool pair_found = true;
        // if(curr_training_entry == NULL)
        if (training_unit.find(key) == training_unit.end())
        {
            // std::cout << "Not found " << std::hex << key << std::endl;
            // TrainingUnitEntry* new_training_entry = training_unit.select(key);
            TrainingUnitEntry *new_training_entry = new TrainingUnitEntry;
            assert(new_training_entry);
            new_training_entry->reset();
            training_unit[key] = new_training_entry;
            pair_found = false;
        }

        assert(training_unit.find(key) != training_unit.end());
        TrainingUnitEntry *curr_training_entry = training_unit.find(key)->second;
        assert(curr_training_entry != NULL);
        last_str_addr = curr_training_entry->str_addr;
        last_phy_addr = curr_training_entry->addr;
        uint64_t last_addr = curr_training_entry->addr;
        if (last_addr == next_addr)
            return false;
#ifdef DEBUG
            // off_chip_corr_matrix.update_neighbor(last_addr, next_addr);
#endif
        return pair_found;
    }

    void update_training_unit(uint64_t key, uint64_t addr, unsigned int str_addr)
    {
        // std::cout << "Updated " << std::hex << key << " to " << addr << std::dec << std::endl;
        assert(training_unit.find(key) != training_unit.end());
        TrainingUnitEntry *curr_training_entry = training_unit.find(key)->second;
        assert(curr_training_entry);
        curr_training_entry->addr = addr;
        curr_training_entry->str_addr = str_addr;
    }

public:
    ISB_prefetcher_t()
    {
        // std::cout<<"ISB constructor constructed\n";
        training_unit.clear();
        alloc_counter = 0;
        last_page = 0;

        stream_divergence_count = 0;
        stream_divergence_new_stream = 0;
        candidate_tlb_miss = 0;
        candidate_diff = 0;
        prefetch_buffer.reset();
    }

    bool get_structural_address(uint64_t addr, unsigned int &str_addr)
    {
        return oci.get_structural_address(addr, str_addr);
    }

    unsigned int assign_structural_addr()
    {
        alloc_counter += STREAM_MAX_LENGTH;
#ifdef DEBUG
        std::cout << "  ALLOC " << alloc_counter << std::endl;
#endif
        return ((unsigned int)alloc_counter);
    }
};

ISB_prefetcher_t isb;

class ISB_Prefetcher
{
    TUCache training_unit;
    OffChipInfo oci;

    uint64_t alloc_counter;
    uint64_t stream_divergence_count;
    uint64_t stream_divergence_new_stream;

    ISB_Prefetcher()
    {
        // std::cout<<"ISB constructor constructed\n";
        training_unit.clear();
        alloc_counter = 0;

        stream_divergence_count = 0;
        stream_divergence_new_stream = 0;
    }
    uint assign_structural_addr()
    {
        alloc_counter += STREAM_MAX_LENGTH;
#ifdef DEBUG
        std::cout << "  ALLOC " << alloc_counter << std::endl;
#endif
        return ((unsigned int)alloc_counter);
    }
    bool get_structural_address(uint64_t addr, uint &str_addr)
    {
        return oci.get_structural_address(addr, str_addr);
    }

    uint isb_train(uint str_addr_A, uint64_t phy_addr_B)
    {
        // Algorithm for training correlated pair (A,B)
        // Step 2a : If SA(A)+1 does not exist, assign B SA(A)+1
        // Step 2b : If SA(A)+1 exists, copy the stream starting at S(A)+1 and then assign B SA(A)+1

        unsigned int str_addr_B;
        bool str_addr_B_exists = oci.get_structural_address(phy_addr_B, str_addr_B);
#ifdef DEBUG
        std::cout << "-----S(A) : " << str_addr_A << std::endl;
#endif
        // If S(A) is at a stream boundary return, we don't need to worry about B because it is as good as a stream start
        if ((str_addr_A + 1) % STREAM_MAX_LENGTH == 0)
        {
            if (!str_addr_B_exists)
            {
                str_addr_B = assign_structural_addr();
                oci.update(phy_addr_B, str_addr_B);
            }
            return str_addr_B;
        }

        bool invalidated = false;
        if (str_addr_B_exists)
        {
            // if(str_addr_B == str_addr_A + 1){
            if (str_addr_B % STREAM_MAX_LENGTH == (str_addr_A + 1) % STREAM_MAX_LENGTH)
            {
#ifdef DEBUG
                std::cout << phy_addr_B << " has a structural address of " << str_addr_B << " conf++ " << std::endl;
#endif
                oci.increase_confidence(phy_addr_B);
                return str_addr_B;
            }
            else
            {
#ifdef DEBUG
                std::cout << phy_addr_B << " has a structural address of " << str_addr_B << " conf-- " << std::endl;
#endif
                bool ret = oci.lower_confidence(phy_addr_B);
                if (ret)
                    return str_addr_B;
#ifdef DEBUG
                std::cout << "Invalidate " << std::endl;
#endif
                // Invalidate the old structural address mapping and assign new
                invalidated = true;
                oci.invalidate(phy_addr_B, str_addr_B);
                str_addr_B_exists = false;
            }
        }

        assert(!str_addr_B_exists);

        // Handle stream divergence

        uint64_t phy_addr_Aplus1;
        bool phy_addr_Aplus1_exists = oci.get_physical_address(phy_addr_Aplus1, str_addr_A + 1);
// #define CFIX
#ifdef CFIX
        if (phy_addr_Aplus1_exists)
        {
            stream_divergence_count++;
            uint i = 0;

            while (phy_addr_Aplus1_exists)
            {
#ifdef DEBUG
                std::cout << "-----S(A)+1 : " << phy_addr_Aplus1 << std::endl;
#endif
                i++;

                if ((str_addr_A + i) % STREAM_MAX_LENGTH == 0)
                {
                    stream_divergence_new_stream++;
                    str_addr_B = assign_structural_addr();
                    break;
                }
                phy_addr_Aplus1_exists = oci.get_physical_address(phy_addr_Aplus1, str_addr_A + i);

                // oci.reassign_stream(str_addr_A+1, assign_structural_addr()); //TODO TBD. Should we re-assign??
            }
            if (!phy_addr_Aplus1_exists)
                str_addr_B = str_addr_A + i;
        }
#else
        if (phy_addr_Aplus1_exists)
        {
            // Old solution: Nothing fancy, just assign a new address
            stream_divergence_count++;
            if (invalidated)
                return str_addr_B;
            else
                str_addr_B = assign_structural_addr();
        }
        else
            str_addr_B = str_addr_A + 1;

#endif

#ifdef DEBUG
        std::cout << (void *)phy_addr_B << " allotted a structural address of " << str_addr_B << std::endl;
        std::cout << "-----S(B) : " << str_addr_B << std::endl;
#endif
        oci.update(phy_addr_B, str_addr_B);

        return str_addr_B;
    }
};

// #define SIG_DEBUG_PRINT				    // Uncomment to turn on Debug Print
#ifdef SIG_DEBUG_PRINT
#define SIG_DP(x) x
#else
#define SIG_DP(x)
#endif

class IP_TABLE_L1
{
public:
    uint64_t ip_tag;
    uint64_t last_vpage;       // last page seen by IP
    uint64_t last_line_offset; // last cl offset in the 4KB page
    int64_t last_stride;       // last stride observed
    uint16_t ip_valid;         // valid bit
    int conf;                  // CS confidence
    uint16_t signature;        // CPLX signature
    uint16_t str_dir;          // stream direction
    uint16_t str_valid;        // stream valid
    uint16_t pref_type;        // pref type or class for book-keeping purposes.

    uint16_t last_str_addr;
    uint64_t last_addr;
    IP_TABLE_L1()
    {
        ip_tag = 0;
        last_vpage = 0;
        last_line_offset = 0;
        last_stride = 0;
        ip_valid = 0;
        signature = 0;
        conf = 0;
        str_dir = 0;
        str_valid = 0;
        pref_type = 0;
    };
};

/*	IP TABLE STORAGE OVERHEAD: 288 Bytes

    Single Entry:

    FIELD					STORAGE (bits)

    IP tag					9
    last page				2
    last line offset			6
    last stride				7 	(6 bits stride + 1 sign bit)
    IP valid				1
    confidence				2
    signature				7
    stream direction			1	1
    stream valid				1

    Total 					36

    Full Table Storage Overhead:

    64 entries * 36 bits = 2304 bits = 288 Bytes

    NOTE: The field prefetch class is used for book-keeping purposes.

*/

class CONST_STRIDE_PRED_TABLE
{
public:
    int stride;
    int conf;

    CONST_STRIDE_PRED_TABLE()
    {
        stride = 0;
        conf = 0;
    };
};

/*	CONSTANT STRIDE STORAGE OVERHEAD: 144 Bytes

    Single Entry:

    FIELD					STORAGE (bits)

    stride					7	(6 bits stride + 1 sign bit)
    confidence 				2

    Total					9

    Full Table Storage Overhead:

    128 entries * 9 bits = 1152 bits = 144 Bytes

*/

/* This class is for bookkeeping purposes only. */
class STAT_COLLECT
{
public:
    uint64_t useful;
    uint64_t filled;
    uint64_t misses;
    uint64_t polluted_misses;

    uint64_t prefetch_reqs;
    uint64_t reqs_filtered_out;

    uint8_t bl_filled[NUM_BLOOM_ENTRIES];
    uint8_t bl_request[NUM_BLOOM_ENTRIES];

    STAT_COLLECT()
    {
        useful = 0;
        filled = 0;
        misses = 0;
        polluted_misses = 0;
        prefetch_reqs = 0;
        reqs_filtered_out = 0;

        for (int i = 0; i < NUM_BLOOM_ENTRIES; i++)
        {
            bl_filled[i] = 0;
            bl_request[i] = 0;
        }
    };
};

class REGION_STREAM_TABLE
{
public:
    uint64_t region_id;
    uint64_t tentative_dense;                    // tentative dense bit
    uint64_t trained_dense;                      // trained dense bit
    uint64_t pos_neg_count;                      // positive/negative stream counter
    uint64_t dir;                                // direction of stream - 1 for +ve and 0 for -ve
    uint64_t lru;                                // lru for replacement
    uint8_t line_access[NUM_OF_LINES_IN_REGION]; // bit vector to store which lines in the 2KB region have been accessed
    REGION_STREAM_TABLE()
    {
        region_id = 0;
        tentative_dense = 0;
        trained_dense = 0;
        pos_neg_count = MAX_POS_NEG_COUNT / 2;
        dir = 0;
        lru = 0;
        for (int i = 0; i < NUM_OF_LINES_IN_REGION; i++)
            line_access[i] = 0;
    };
};

/*	REGION STREAM TABLE STORAGE OVERHEAD:

    Single Entry:

    FIELD					STORAGE (bits)

    region id				3
    tentative dense				1
    trained dense				1
    positive/negative count			6
    direction				1
    lru 					3
    bit vector line access			32	(for 2KB region)

    Total					47

    Full Table Storage Overhead:

    8 entries * 47 bits = 376 bits = 47 Bytes

*/

REGION_STREAM_TABLE rstable[NUM_CPUS][NUM_RST_ENTRIES];
int acc_filled[NUM_CPUS][NUM_CLASSES];
int acc_useful[NUM_CPUS][NUM_CLASSES];

int acc[NUM_CPUS][NUM_CLASSES];
int prefetch_degree[NUM_CPUS][NUM_CLASSES];
int num_conflicts = 0;
int test;

uint64_t eval_buffer[NUM_CPUS][1024] = {};
STAT_COLLECT stats[NUM_CPUS][NUM_CLASSES]; // for GS, CS, CPLX, NL and no class
IP_TABLE_L1 trackers_l1[NUM_CPUS][NUM_IP_TABLE_L1_ENTRIES];
CONST_STRIDE_PRED_TABLE CSPT_l1[NUM_CPUS][NUM_CSPT_ENTRIES];

vector<uint64_t> recent_request_filter; // to filter redundant prefetch requests

/* 	RECENT REQUEST FILTER STORAGE OVERHEAD: 48 Bytes

    FIELD					STORAGE (bits)

    Tag					12

    Total Storage Overhead:

    32 entries * 12 bits = 384 bits = 48 Bytes

*/

uint64_t prev_cpu_cycle[NUM_CPUS];
uint64_t num_misses[NUM_CPUS];
float mpki[NUM_CPUS] = {0};
int spec_nl[NUM_CPUS] = {0}, flag_nl[NUM_CPUS] = {0};
uint64_t num_access[NUM_CPUS];

int meta_counter[NUM_CPUS][NUM_CLASSES] = {0}; // for book-keeping
int total_count[NUM_CPUS] = {0};               // for book-keeping

/* update_sig_l1: 7 bit signature is updated by performing a left-shift of 1 bit on the old signature and xoring the outcome with the delta*/

uint16_t update_sig_l1(uint16_t old_sig, int delta)
{
    uint16_t new_sig = 0;
    int sig_delta = 0;

    // 7-bit sign magnitude form, since we need to track deltas from +63 to -63
    sig_delta = (delta < 0) ? (((-1) * delta) + (1 << 6)) : delta;
    new_sig = ((old_sig << 1) ^ sig_delta) & ((1 << NUM_SIG_BITS) - 1);

    return new_sig;
}

/* encode_metadata: The stride, prefetch class type and speculative nl fields are encoded in the metadata. */

uint32_t encode_metadata(int stride, uint16_t type, int spec_nl)
{
    uint32_t metadata = 0;

    // first encode stride in the last 8 bits of the metadata
    if (stride > 0)
        metadata = stride;
    else
        metadata = ((-1 * stride) | 0b1000000);

    // encode the type of IP in the next 4 bits
    metadata = metadata | (type << 8);

    // encode the speculative NL bit in the next 1 bit
    metadata = metadata | (spec_nl << 12);

    return metadata;
}

/*If the actual stride and predicted stride are equal, then the confidence counter is incremented. */

int update_conf(int stride, int pred_stride, int conf)
{
    if (stride == pred_stride)
    { // use 2-bit saturating counter for confidence
        conf++;
        if (conf > 3)
            conf = 3;
    }
    else
    {
        conf--;
        if (conf < 0)
            conf = 0;
    }

    return conf;
}

uint64_t hash_bloom(uint64_t addr)
{
    uint64_t first_half, sec_half;
    first_half = addr & 0xFFF;
    sec_half = (addr >> 12) & 0xFFF;
    if ((first_half ^ sec_half) >= 4096)
        assert(0);
    return ((first_half ^ sec_half) & 0xFFF);
}

uint64_t hash_page(uint64_t addr)
{
    uint64_t hash;
    while (addr != 0)
    {
        hash = hash ^ addr;
        addr = addr >> 6;
    }

    return hash & ((1 << NUM_PAGE_TAG_BITS) - 1);
}

void stat_col_L1(uint64_t addr, uint8_t cache_hit, uint8_t cpu, uint64_t ip)
{
    uint64_t index = hash_bloom(addr);
    int ip_index = ip & ((1 << NUM_IP_INDEX_BITS) - 1);
    uint16_t ip_tag = (ip >> NUM_IP_INDEX_BITS) & ((1 << NUM_IP_TAG_BITS) - 1);

    for (int i = 0; i < NUM_CLASSES; i++)
    {
        if (cache_hit)
        {
            if (stats[cpu][i].bl_filled[index] == 1)
            {
                stats[cpu][i].useful++;
                stats[cpu][i].filled++;
                stats[cpu][i].bl_filled[index] = 0;
            }
        }
        else
        {
            if (ip_tag == trackers_l1[cpu][ip_index].ip_tag)
            {
                if (trackers_l1[cpu][ip_index].pref_type == i)
                    stats[cpu][i].misses++;
                if (stats[cpu][i].bl_filled[index] == 1)
                {
                    stats[cpu][i].polluted_misses++;
                    stats[cpu][i].filled++;
                    stats[cpu][i].bl_filled[index] = 0;
                }
            }
        }

        if (num_misses[cpu] % 1024 == 0)
        {
            for (int j = 0; j < NUM_BLOOM_ENTRIES; j++)
            {
                stats[cpu][i].filled += stats[cpu][i].bl_filled[j];
                stats[cpu][i].bl_filled[j] = 0;
                stats[cpu][i].bl_request[j] = 0;
            }
        }
    }
}

void CACHE::l1d_prefetcher_initialize()
{
    for (int i = 0; i < NUM_RST_ENTRIES; i++)
        rstable[cpu][i].lru = i;
    for (int i = 0; i < NUM_CPUS; i++)
    {
        prefetch_degree[cpu][0] = 0;
        prefetch_degree[cpu][1] = 6;
        prefetch_degree[cpu][2] = 3;
        prefetch_degree[cpu][3] = 3;
        prefetch_degree[cpu][4] = 1;
        prefetch_degree[cpu][5] = 6;
    }
}

void CACHE::l1d_prefetcher_operate(uint64_t addr, uint64_t ip, uint8_t cache_hit, uint8_t type, uint8_t critical_ip_flag)
{

    uint64_t curr_page = hash_page(addr >> LOG2_PAGE_SIZE);  // current page
    uint64_t line_addr = addr >> LOG2_BLOCK_SIZE;            // cache line address
    uint64_t line_offset = (addr >> LOG2_BLOCK_SIZE) & 0x3F; // cache line offset
    uint16_t signature = 0, last_signature = 0;
    int spec_nl_threshold = 0;
    int num_prefs = 0;      // Global count of all prefetches made by all prefetchers
    uint32_t metadata = 0;
    uint16_t ip_tag = (ip >> NUM_IP_INDEX_BITS) & ((1 << NUM_IP_TAG_BITS) - 1);
    uint64_t bl_index = 0;

    if (NUM_CPUS == 1)
    {
        spec_nl_threshold = 50;
    }
    else
    { // tightening the mpki constraints for multi-core
        spec_nl_threshold = 40;
    }

    // update miss counter
    if (cache_hit == 0 && warmup_complete[cpu] == 1)
        num_misses[cpu] += 1;
    num_access[cpu] += 1;
    stat_col_L1(addr, cache_hit, cpu, ip);
    // update spec nl bit when num misses crosses certain threshold
    if (num_misses[cpu] % 256 == 0 && cache_hit == 0)
    {
        mpki[cpu] = ((num_misses[cpu] * 1000.0) / (ooo_cpu[cpu].num_retired - ooo_cpu[cpu].warmup_instructions));

        if (mpki[cpu] > spec_nl_threshold)
            spec_nl[cpu] = 0;
        else
            spec_nl[cpu] = 1;
    }

    // Updating prefetch degree based on accuracy
    for (int i = 0; i < 5 ; i++)
    {
        if (pref_filled[cpu][i] % 256 == 0)
        {

            acc_useful[cpu][i] = acc_useful[cpu][i] / 2.0 + (pref_useful[cpu][i] - acc_useful[cpu][i]) / 2.0;
            acc_filled[cpu][i] = acc_filled[cpu][i] / 2.0 + (pref_filled[cpu][i] - acc_filled[cpu][i]) / 2.0;

            if (acc_filled[cpu][i] != 0)
                acc[cpu][i] = 100.0 * acc_useful[cpu][i] / (acc_filled[cpu][i]);
            else
                acc[cpu][i] = 60;

            if (acc[cpu][i] > 75)
            {
                prefetch_degree[cpu][i]++;
                if (i == 1)
                {
                    // For GS class, degree is incremented/decremented by 2.
                    prefetch_degree[cpu][i]++;
                    if (prefetch_degree[cpu][i] > 6)
                        prefetch_degree[cpu][i] = 6;
                }
                else if (prefetch_degree[cpu][i] > 3)
                    prefetch_degree[cpu][i] = 3;
            }
            else if (acc[cpu][i] < 40)
            {
                prefetch_degree[cpu][i]--;
                if (i == 1)
                    prefetch_degree[cpu][i]--;
                if (prefetch_degree[cpu][i] < 1)
                    prefetch_degree[cpu][i] = 1;
            }
        }
    }

    // calculate the index bit
    int index = ip & ((1 << NUM_IP_INDEX_BITS) - 1);
    if (trackers_l1[cpu][index].ip_tag != ip_tag)
    { // new/conflict IP
        if (trackers_l1[cpu][index].ip_valid == 0)
        { // if valid bit is zero, update with latest IP info
            num_conflicts++;
            trackers_l1[cpu][index].ip_tag = ip_tag;
            trackers_l1[cpu][index].last_vpage = curr_page;
            trackers_l1[cpu][index].last_line_offset = line_offset;
            trackers_l1[cpu][index].last_stride = 0;
            trackers_l1[cpu][index].signature = 0;
            trackers_l1[cpu][index].conf = 0;
            trackers_l1[cpu][index].str_valid = 0;
            trackers_l1[cpu][index].str_dir = 0;
            trackers_l1[cpu][index].pref_type = 0;
            trackers_l1[cpu][index].ip_valid = 1;
        }
        else
        { // otherwise, reset valid bit and leave the previous IP as it is
            trackers_l1[cpu][index].ip_valid = 0;
        }

        return;
    }
    else
    { // if same IP encountered, set valid bit
        trackers_l1[cpu][index].ip_valid = 1;
    }

    // calculate the stride between the current cache line offset and the last cache line offset
    int64_t stride = 0;
    if (line_offset > trackers_l1[cpu][index].last_line_offset)
        stride = line_offset - trackers_l1[cpu][index].last_line_offset;
    else
    {
        stride = trackers_l1[cpu][index].last_line_offset - line_offset;
        stride *= -1;
    }

    // don't do anything if same address is seen twice in a row
    if (stride == 0)
        return;

    uint64_t addr_B = (addr >> 6) << 6;
    uint64_t key = ip;

    if (addr_B == last_address)
        return;
    last_address = addr_B;

    total_access++;

#ifdef DEBUG
    std::cout << "**Trigger " << std::hex << addr_B << " with key " << std::hex << key << std::endl;
#endif

    unsigned int str_addr_B = 0;
    bool str_addr_B_exists = isb.oci.get_structural_address(addr_B, str_addr_B);

    int c = 0, flag = 0;
    bool gs_selected = false, cs_selected = false, cplx_selected = false;

    // Checking if IP is already classified as a part of the GS class, so that for the new region we will set the tentative (spec_dense) bit.
    for (int i = 0; i < NUM_RST_ENTRIES; i++)
    {
        if (rstable[cpu][i].region_id == ((trackers_l1[cpu][index].last_vpage << 1) | (trackers_l1[cpu][index].last_line_offset >> 5)))
        {
            if (rstable[cpu][i].trained_dense == 1)
                flag = 1;
            break;
        }
    }
    for (c = 0; c < NUM_RST_ENTRIES; c++)
    {
        if (((curr_page << 1) | (line_offset >> 5)) == rstable[cpu][c].region_id)
        {
            if (rstable[cpu][c].line_access[line_offset & REGION_OFFSET_MASK] == 0)
            {
                rstable[cpu][c].line_access[line_offset & REGION_OFFSET_MASK] = 1;
            }

            if (rstable[cpu][c].pos_neg_count >= MAX_POS_NEG_COUNT || rstable[cpu][c].pos_neg_count <= 0)
            {
                rstable[cpu][c].pos_neg_count = MAX_POS_NEG_COUNT / 2;
            }

            if (stride > 0)
                rstable[cpu][c].pos_neg_count++;
            else
                rstable[cpu][c].pos_neg_count--;

            if (rstable[cpu][c].trained_dense == 0)
            {
                int count = 0;
                for (int i = 0; i < NUM_OF_LINES_IN_REGION; i++)
                    if (rstable[cpu][c].line_access[line_offset & REGION_OFFSET_MASK] == 1)
                        count++;

                if (count > 24) // 75% of the cache lines in the region are accessed.
                {
                    rstable[cpu][c].trained_dense = 1;
                }
            }
            if (flag == 1)
                rstable[cpu][c].tentative_dense = 1;
            if (rstable[cpu][c].tentative_dense == 1 || rstable[cpu][c].trained_dense == 1)
            {
                if (rstable[cpu][c].pos_neg_count > (MAX_POS_NEG_COUNT / 2))
                    rstable[cpu][c].dir = 1; // 1 for positive direction
                else
                    rstable[cpu][c].dir = 0; // 0 for negative direction
                trackers_l1[cpu][index].str_valid = 1;

                trackers_l1[cpu][index].str_dir = rstable[cpu][c].dir;
            }
            else
                trackers_l1[cpu][index].str_valid = 0;

            break;
        }
    }
    // curr page has no entry in rstable. Then replace lru.
    if (c == NUM_RST_ENTRIES)
    {
        // check lru
        for (c = 0; c < NUM_RST_ENTRIES; c++)
        {
            if (rstable[cpu][c].lru == (NUM_RST_ENTRIES - 1))
                break;
        }
        for (int i = 0; i < NUM_RST_ENTRIES; i++)
        {
            if (rstable[cpu][i].lru < rstable[cpu][c].lru)
                rstable[cpu][i].lru++;
        }
        if (flag == 1)
            rstable[cpu][c].tentative_dense = 1;
        else
            rstable[cpu][c].tentative_dense = 0;

        rstable[cpu][c].region_id = (curr_page << 1) | (line_offset >> 5);
        rstable[cpu][c].trained_dense = 0;
        rstable[cpu][c].pos_neg_count = MAX_POS_NEG_COUNT / 2;
        rstable[cpu][c].dir = 0;
        rstable[cpu][c].lru = 0;
        for (int i = 0; i < NUM_OF_LINES_IN_REGION; i++)
            rstable[cpu][c].line_access[i] = 0;
    }

    // page boundary learning
    if (curr_page != trackers_l1[cpu][index].last_vpage)
    {
        test++;
        if (stride < 0)
            stride += NUM_OF_LINES_IN_REGION;
        else
            stride -= NUM_OF_LINES_IN_REGION;
    }

    // update constant stride(CS) confidence
    trackers_l1[cpu][index].conf = update_conf(stride, trackers_l1[cpu][index].last_stride, trackers_l1[cpu][index].conf);

    // update CS only if confidence is zero
    if (trackers_l1[cpu][index].conf == 0)
        trackers_l1[cpu][index].last_stride = stride;

    last_signature = trackers_l1[cpu][index].signature;
    // update complex stride(CPLX) confidence
    CSPT_l1[cpu][last_signature].conf = update_conf(stride, CSPT_l1[cpu][last_signature].stride, CSPT_l1[cpu][last_signature].conf);

    // update CPLX only if confidence is zero
    if (CSPT_l1[cpu][last_signature].conf == 0)
        CSPT_l1[cpu][last_signature].stride = stride;

    // calculate and update new signature in IP table
    signature = update_sig_l1(last_signature, stride);
    trackers_l1[cpu][index].signature = signature;

    SIG_DP(
        cout << ip << ", " << cache_hit << ", " << line_addr << ", " << addr << ", " << stride << "; ";
        cout << last_signature << ", " << CSPT_l1[cpu][last_signature].stride << ", " << CSPT_l1[cpu][last_signature].conf << "; ";
        cout << trackers_l1[cpu][index].last_stride << ", " << stride << ", " << trackers_l1[cpu][index].conf << ", " << "; ";);

    // cout << "IP: " << ip << " ";
    if (trackers_l1[cpu][index].str_valid == 1)
    { // stream IP
        // for stream, prefetch with twice the usual degree
        if (prefetch_degree[cpu][1] < 3)
            flag = 1;
        meta_counter[cpu][0]++;
        total_count[cpu]++;
        for (int i = 0; i < prefetch_degree[cpu][1]; i++)
        {
            uint64_t pf_address = 0;

            if (trackers_l1[cpu][index].str_dir == 1)
            { // +ve stream
                pf_address = (line_addr + i + 1) << LOG2_BLOCK_SIZE;
                metadata = encode_metadata(1, S_TYPE, spec_nl[cpu]); // stride is 1
            }
            else
            { // -ve stream
                pf_address = (line_addr - i - 1) << LOG2_BLOCK_SIZE;
                metadata = encode_metadata(-1, S_TYPE, spec_nl[cpu]); // stride is -1
            }

            if (acc[cpu][1] < 75)
                metadata = encode_metadata(0, S_TYPE, spec_nl[cpu]);
            // Check if prefetch address is in same 4 KB page
            if ((pf_address >> LOG2_PAGE_SIZE) != (addr >> LOG2_PAGE_SIZE))
            {
                break;
            }

            trackers_l1[cpu][index].pref_type = S_TYPE;

#ifdef DO_PREF
            int found_in_filter = 0;
            stats[cpu][S_TYPE].prefetch_reqs++;
            for (int i = 0; i < recent_request_filter.size(); i++)
            {
                if (recent_request_filter[i] == ((pf_address >> 6) & RR_TAG_MASK))
                {
                    // Prefetch address is present in RR filter
                    found_in_filter = 1;
                    break;
                }
            }
            // Issue prefetch request only if prefetch address is not present in RR filter
            if (found_in_filter == 0)
            {
                prefetch_line(ip, addr, pf_address, FILL_L1, metadata);

                // Add to RR filter
                recent_request_filter.push_back((pf_address >> 6) & RR_TAG_MASK);
                if (recent_request_filter.size() > NUM_OF_RR_ENTRIES)
                    recent_request_filter.erase(recent_request_filter.begin());
            }
            else{
                stats[cpu][S_TYPE].reqs_filtered_out++;
            }
#endif
            num_prefs++;
            SIG_DP(cout << "1, ");
        }
        gs_selected = true;
        // cout << " stream selected. Flag: " << flag << endl;
    }
    else
        flag = 1;

    if (trackers_l1[cpu][index].conf > 1 && trackers_l1[cpu][index].last_stride != 0 && flag == 1)
    { // CS IP
        meta_counter[cpu][1]++;
        total_count[cpu]++;

        if (prefetch_degree[cpu][2] < 2)
            flag = 1;
        else
            flag = 0;

        for (int i = 0; i < prefetch_degree[cpu][2]; i++)
        {
            uint64_t pf_address = (line_addr + (trackers_l1[cpu][index].last_stride * (i + 1))) << LOG2_BLOCK_SIZE;
            stats[cpu][CS_TYPE].prefetch_reqs++;
            // Check if prefetch address is in same 4 KB page
            if ((pf_address >> LOG2_PAGE_SIZE) != (addr >> LOG2_PAGE_SIZE))
            {
                break;
            }

            trackers_l1[cpu][index].pref_type = CS_TYPE;
            bl_index = hash_bloom(pf_address);
            stats[cpu][CS_TYPE].bl_request[bl_index] = 1;
            if (acc[cpu][2] > 75)
                metadata = encode_metadata(trackers_l1[cpu][index].last_stride, CS_TYPE, spec_nl[cpu]);
            else
                metadata = encode_metadata(0, CS_TYPE, spec_nl[cpu]);
// if(spec_nl[cpu] == 1)
#ifdef DO_PREF
            int found_in_filter = 0;
            for (int i = 0; i < recent_request_filter.size(); i++)
            {
                if (recent_request_filter[i] == ((pf_address >> 6) & RR_TAG_MASK))
                {
                    // Prefetch address is present in RR filter
                    found_in_filter = 1;
                }
            }
            // Issue prefetch request only if prefetch address is not present in RR filter
            if (found_in_filter == 0)
            {
                prefetch_line(ip, addr, pf_address, FILL_L1, metadata);
                // Add to RR filter
                recent_request_filter.push_back((pf_address >> 6) & RR_TAG_MASK);
                if (recent_request_filter.size() > NUM_OF_RR_ENTRIES)
                    recent_request_filter.erase(recent_request_filter.begin());
            }
            else{
                stats[cpu][CS_TYPE].reqs_filtered_out++;
            }
#endif
            num_prefs++;
            SIG_DP(cout << trackers_l1[cpu][index].last_stride << ", ");
        }
        cs_selected = true;
        // cout << " ip stride selected. Flag: " << flag << endl;
    }
    else
        flag = 1;

    if (CSPT_l1[cpu][signature].conf >= 0 && CSPT_l1[cpu][signature].stride != 0 && flag == 1)
    {                               // if conf>=0, continue looking for stride
        int pref_offset = 0, i = 0; // CPLX IP
        meta_counter[cpu][2]++;
        total_count[cpu]++;
        if (prefetch_degree[cpu][3] < 2)
            flag = 1;
        else
            flag = 0;

        for (i = 0; i < prefetch_degree[cpu][3] + CPLX_DIST; i++)
        {
            pref_offset += CSPT_l1[cpu][signature].stride;
            uint64_t pf_address = ((line_addr + pref_offset) << LOG2_BLOCK_SIZE);
            
            // Check if prefetch address is in same 4 KB page
            if (((pf_address >> LOG2_PAGE_SIZE) != (addr >> LOG2_PAGE_SIZE)) ||
                (CSPT_l1[cpu][signature].conf == -1) ||
                (CSPT_l1[cpu][signature].stride == 0))
            {
                // if new entry in CSPT or stride is zero, break
                break;
            }

            // we are not prefetching at L2 for CPLX type, so encode stride as 0
            trackers_l1[cpu][index].pref_type = CPLX_TYPE;
            metadata = encode_metadata(0, CPLX_TYPE, spec_nl[cpu]);
            if (CSPT_l1[cpu][signature].conf > 0 && i >= CPLX_DIST)
            { // prefetch only when conf>0 for CPLX
                bl_index = hash_bloom(pf_address);
                stats[cpu][CPLX_TYPE].bl_request[bl_index] = 1;
                trackers_l1[cpu][index].pref_type = 3;
#ifdef DO_PREF
                int found_in_filter = 0;
                for (int i = 0; i < recent_request_filter.size(); i++)
                {
                    if (recent_request_filter[i] == ((pf_address >> 6) & RR_TAG_MASK))
                    {
                        // Prefetch address is present in RR filter
                        found_in_filter = 1;
                    }
                }
                stats[cpu][CPLX_TYPE].prefetch_reqs++;
                // Issue prefetch request only if prefetch address is not present in RR filter
                if (found_in_filter == 0)
                {
                    prefetch_line(ip, addr, pf_address, FILL_L1, metadata);
                    // Add to RR filter
                    recent_request_filter.push_back((pf_address >> 6) & RR_TAG_MASK);
                    if (recent_request_filter.size() > NUM_OF_RR_ENTRIES)
                        recent_request_filter.erase(recent_request_filter.begin());
                }
                else{
                    stats[cpu][CPLX_TYPE].reqs_filtered_out++;
                }
#endif
                num_prefs++;
                SIG_DP(cout << pref_offset << ", ");
            }
            signature = update_sig_l1(signature, CSPT_l1[cpu][signature].stride);
        }
        cplx_selected = true;
        // cout << " complex stride selected. Flag: " << flag << endl;
    }
    else
        flag = 1;

    if (str_addr_B_exists && flag == 1)
    {

        // if (gs_selected)
        // {
        meta_counter[cpu][4]++;
        total_count[cpu]++;
        uint num_prefetched = 0; // local count of number of prefetches made
        // if (prefetch_degree[cpu][5] > 3)
        // {
        //     prefetch_degree[cpu][5] = 3;
        // }

        vector<uint64_t> candidates = isb.isb_predict(addr_B, str_addr_B, ip, prefetch_degree[cpu][5]);

        trackers_l1[cpu][index].pref_type = IA_TYPE;
        for (unsigned int i = 0; i < candidates.size(); i++)
        {
            metadata = encode_metadata(0, IA_TYPE, spec_nl[cpu]);
            bl_index = hash_bloom(candidates[i]);
            stats[cpu][IA_TYPE].bl_request[bl_index] = 1;
#ifdef DO_PREF
            int found_in_filter = 0;
            for (int j = 0; j < recent_request_filter.size(); j++)
            {
                if (recent_request_filter[j] == ((candidates[i] >> 6) & RR_TAG_MASK))
                {
                    // Prefetch address is present in RR filter
                    found_in_filter = 1;
                }
            }
            stats[cpu][IA_TYPE].prefetch_reqs++;
            // Issue prefetch request only if prefetch address is not present in RR filter
            if (found_in_filter == 0)
            {
                // auto conf = isb.oci.get_confidence(candidates[i]);
                // if (conf == 3)
                // {
                int ret = prefetch_line(ip, addr, candidates[i], FILL_L1, metadata);
                if (ret == 1)
                {
                    predictions++;
                    num_prefetched++;
                    num_prefs++;
                }
                recent_request_filter.push_back((candidates[i] >> 6) & RR_TAG_MASK);
                if (recent_request_filter.size() > NUM_OF_RR_ENTRIES)
                    recent_request_filter.erase(recent_request_filter.begin());

                if (num_prefetched >= prefetch_degree[cpu][5])
                    break;
                // }
            }
            else{
                stats[cpu][IA_TYPE].reqs_filtered_out++;
            }
#endif
        }
        // }

        // cout << " isb selected. Flag: " << flag << endl;
    }
    else
        no_prediction++;

    unsigned int str_addr_A;
    uint64_t addr_A;
    if (isb.access_training_unit(key, addr_A, str_addr_A, addr_B))
    {
#ifdef DEBUG
        std::cout << "Consider pair " << str_addr_A << " and " << addr_B << " with key as " << key << std::endl;
#endif
        if (str_addr_A == 0)
        { // TBD, when is this condition true? When this is the 2nd access to the pc
            str_addr_A = isb.assign_structural_addr();
            isb.oci.update(addr_A, str_addr_A);
        }
        str_addr_B = isb.isb_train(str_addr_A, addr_B);
    }

    isb.update_training_unit(key, addr_B, str_addr_B);

    // if no prefetches are issued till now, speculatively issue a next_line prefetch
    if (num_prefs == 0 && spec_nl[cpu] == 1)
    {
        if (flag_nl[cpu] == 0)
            flag_nl[cpu] = 1;
        else
        {
            uint64_t pf_address = ((addr >> LOG2_BLOCK_SIZE) + 1) << LOG2_BLOCK_SIZE;
            if ((pf_address >> LOG2_PAGE_SIZE) != (addr >> LOG2_PAGE_SIZE))
            {
                // update the IP table entries and return if NL request is not to the same 4 KB page
                trackers_l1[cpu][index].last_line_offset = line_offset;
                trackers_l1[cpu][index].last_vpage = curr_page;
                return;
            }
            bl_index = hash_bloom(pf_address);
            stats[cpu][NL_TYPE].bl_request[bl_index] = 1;
            metadata = encode_metadata(1, NL_TYPE, spec_nl[cpu]);
#ifdef DO_PREF
            int found_in_filter = 0;
            stats[cpu][NL_TYPE].prefetch_reqs++;
            for (int i = 0; i < recent_request_filter.size(); i++)
            {
                if (recent_request_filter[i] == ((pf_address >> 6) & RR_TAG_MASK))
                {
                    // Prefetch address is present in RR filter
                    found_in_filter = 1;
                }
            }
            // Issue prefetch request only if prefetch address is not present in RR filter
            if (found_in_filter == 0)
            {
                prefetch_line(ip, addr, pf_address, FILL_L1, metadata);
                // Add to RR filter
                recent_request_filter.push_back((pf_address >> 6) & RR_TAG_MASK);
                if (recent_request_filter.size() > NUM_OF_RR_ENTRIES)
                    recent_request_filter.erase(recent_request_filter.begin());
            }
            else{
                stats[cpu][NL_TYPE].reqs_filtered_out++;
            }
#endif
            trackers_l1[cpu][index].pref_type = NL_TYPE;
            meta_counter[cpu][3]++;
            total_count[cpu]++;
            SIG_DP(cout << "1, ");

            if (acc[cpu][4] < 40)
                flag_nl[cpu] = 0;

            // cout << " next line selected. Flag: " << flag << endl;
        } // NL IP
    }

    SIG_DP(cout << endl);

    // update the IP table entries
    trackers_l1[cpu][index].last_line_offset = line_offset;
    trackers_l1[cpu][index].last_vpage = curr_page;

    return;
}

void CACHE::l1d_prefetcher_cache_fill(uint64_t v_addr, uint64_t addr, uint32_t set, uint32_t way, uint8_t prefetch, uint64_t v_evicted_addr, uint64_t evicted_addr, uint32_t metadata_in)
{

    if (prefetch)
    {
        uint32_t pref_type = metadata_in & 0xF00;
        pref_type = pref_type >> 8;

        uint64_t index = hash_bloom(addr);
        if (stats[cpu][pref_type].bl_request[index] == 1)
        {
            stats[cpu][pref_type].bl_filled[index] = 1;
            stats[cpu][pref_type].bl_request[index] = 0;
        }
    }
}
void CACHE::l1d_prefetcher_final_stats()
{
    cout << endl;

    uint64_t total_request = 0, total_polluted = 0, total_useful = 0, total_late = 0;

    for (int i = 0; i < NUM_CLASSES; i++)
    {
        total_request += pref_filled[cpu][i];
        total_polluted += stats[cpu][i].polluted_misses;
        total_useful += pref_useful[cpu][i];
        total_late += pref_late[cpu][i];
    }

    cout << "stream: " << endl;
    cout << "stream:times selected: " << meta_counter[cpu][0] << endl;
    cout << "stream:pref_reqs: " << stats[cpu][S_TYPE].prefetch_reqs << endl;
    cout << "stream:pref_reqs_filtered_out: " << stats[cpu][S_TYPE].reqs_filtered_out << endl;
    cout << "stream:pref_filled: " << pref_filled[cpu][1] << endl;
    cout << "stream:pref_useful: " << pref_useful[cpu][1] << endl;
    cout << "stream:pref_late: " << pref_late[cpu][1] << endl;
    cout << "stream:misses: " << stats[cpu][1].misses << endl;
    cout << "stream:misses_by_poll: " << stats[cpu][1].polluted_misses << endl;
    cout << endl;

    cout << "CS: " << endl;
    cout << "CS:times selected: " << meta_counter[cpu][1] << endl;
    cout << "CS:pref_reqs: " << stats[cpu][CS_TYPE].prefetch_reqs << endl;
    cout << "CS:pref_reqs_filtered_out: " << stats[cpu][CS_TYPE].reqs_filtered_out << endl;
    cout << "CS:pref_filled: " << pref_filled[cpu][2] << endl;
    cout << "CS:pref_useful: " << pref_useful[cpu][2] << endl;
    cout << "CS:pref_late: " << pref_late[cpu][2] << endl;
    cout << "CS:misses: " << stats[cpu][2].misses << endl;
    cout << "CS:misses_by_poll: " << stats[cpu][2].polluted_misses << endl;
    cout << endl;

    cout << "CPLX: " << endl;
    cout << "CPLX:times selected: " << meta_counter[cpu][2] << endl;
    cout << "CPLX:pref_reqs: " << stats[cpu][CPLX_TYPE].prefetch_reqs << endl;
    cout << "CPLX:pref_reqs_filtered_out: " << stats[cpu][CPLX_TYPE].reqs_filtered_out << endl;
    cout << "CPLX:pref_filled: " << pref_filled[cpu][3] << endl;
    cout << "CPLX:pref_useful: " << pref_useful[cpu][3] << endl;
    cout << "CPLX:pref_late: " << pref_late[cpu][3] << endl;
    cout << "CPLX:misses: " << stats[cpu][3].misses << endl;
    cout << "CPLX:misses_by_poll: " << stats[cpu][3].polluted_misses << endl;
    cout << endl;

    cout << "ISB: " << endl;
    cout << "ISB:times selected:" << meta_counter[cpu][4] << endl;
    cout << "ISB:pref_reqs: " << stats[cpu][IA_TYPE].prefetch_reqs << endl;
    cout << "ISB:pref_reqs_filtered_out: " << stats[cpu][IA_TYPE].reqs_filtered_out << endl;
    cout << "ISB:pref_filled: " << pref_filled[cpu][5] << endl;
    cout << "ISB:pref_useful: " << pref_useful[cpu][5] << endl;
    cout << "ISB:pref_late: " << pref_late[cpu][5] << endl;
    cout << "ISB:misses: " << stats[cpu][5].misses << endl;
    cout << "ISB:misses_by_poll: " << stats[cpu][5].polluted_misses << endl;

    cout << "ISB:Stream end: " << stream_end << endl;
    cout << "ISB:No Prediction: " << no_prediction << " " << 100 * (double)no_prediction / (double)total_access << endl;
    cout << "ISB:No translation: " << no_translation << " " << 100 * (double)no_translation / (double)total_access << endl;
    cout << "ISB:Predictions: " << predictions << " " << 100 * (double)predictions / (double)total_access << endl;

    cout << endl;

    cout << "NL_L1: " << endl;
    cout << "NL:times selected: " << meta_counter[cpu][3] << endl;
    cout << "NL:pref_reqs: " << stats[cpu][NL_TYPE].prefetch_reqs << endl;
    cout << "NL:pref_reqs_filtered_out: " << stats[cpu][NL_TYPE].reqs_filtered_out << endl;
    cout << "NL:pref_filled: " << pref_filled[cpu][4] << endl;
    cout << "NL:pref_useful: " << pref_useful[cpu][4] << endl;
    cout << "NL:pref_late: " << pref_late[cpu][4] << endl;
    cout << "NL:misses: " << stats[cpu][4].misses << endl;
    cout << "NL:misses_by_poll: " << stats[cpu][4].polluted_misses << endl;
    cout << endl;

    cout << "total selections: " << total_count[cpu] << endl;
    cout << "total_filled: " << pf_fill << endl;
    cout << "total_useful: " << pf_useful << endl;
    cout << "total_late: " << total_late << endl;
    cout << "total_polluted: " << total_polluted << endl;
    cout << "total_misses_after_warmup: " << num_misses[cpu] << endl;

    cout << "conflicts: " << num_conflicts << endl;
    cout << endl;

    cout << "test: " << test << endl;
}