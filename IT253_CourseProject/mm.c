#include <stdio.h>
#include <memory.h>
#include <unistd.h>     //for getpagesize
#include <sys/mman.h>   //For using mmap()
#include <stdint.h>
#include "mm.h"
#include <assert.h>
#include "css.h"
#include "uapi_mm.h"
#include <stdlib.h>
//starting page is set to null initially
static vm_page_for_families_t *first_vm_page_for_families = NULL;
static size_t SYSTEM_PAGE_SIZE = 0;

typedef struct Allocation {
    void* ptr;
    size_t size;
    int freed; 
    struct Allocation* next;
} Allocation;


Allocation* head = NULL;

void mm_init(){

    SYSTEM_PAGE_SIZE = getpagesize();//returns size of one page
}
//accepts as argument the number of units of contiguous free memory location
//to find the free space multiply the number of units and page size - the meta data 
static inline uint32_t
mm_max_page_allocatable_memory(int units){

    return (uint32_t)
        ((SYSTEM_PAGE_SIZE * units) - offset_of(vm_page_t, page_memory));
}

#define MAX_PAGE_ALLOCATABLE_MEMORY(units) \
    (mm_max_page_allocatable_memory(units))


//Function to request Virtual memory page from kernel
static void * mm_get_new_vm_page_from_kernel(int units){

    //mmap() system call returns the address of the starting of the allocatted page
    //read,write and execute are permissions
    char *vm_page = mmap(0, units * SYSTEM_PAGE_SIZE,
        PROT_READ|PROT_WRITE|PROT_EXEC,
        MAP_ANON|MAP_PRIVATE,
        0, 0);

    //error handling
    if(vm_page == MAP_FAILED){
        printf("Error : VM Page allocation Failed\n");
        return NULL;
    }
    //initialise the memory assigned by mmap to our user process using memset()
    memset(vm_page, 0, units * SYSTEM_PAGE_SIZE);
    return (void *)vm_page;
}

//Function to return a page to kernel
static void
mm_return_vm_page_to_kernel (void *vm_page, int units){

    //munmap system allocatte to return the page and also error handling
    if(munmap(vm_page, units * SYSTEM_PAGE_SIZE)){
        printf("Error : Could not munmap VM page to kernel");
    }
}

//theres a hard internally fragmented metablock sandwiched between 2 free meta blocks first and second(returns0 if no internal fragmented blocks)
static int
mm_get_hard_internal_memory_frag_size(
        block_meta_data_t *first,
        block_meta_data_t *second){

    block_meta_data_t *next_block = NEXT_META_BLOCK_BY_SIZE(first);
    return (int)((unsigned long)second - (unsigned long)(next_block));
}

//to join the free consecutive blocks

static void
mm_union_free_blocks(block_meta_data_t *first,
        block_meta_data_t *second){

    //the two blocks should be marked as free
    assert(first->is_free == MM_TRUE &&
            second->is_free == MM_TRUE);
    //update data block size
    first->block_size += sizeof(block_meta_data_t) +
        second->block_size;
    //update first meta block's next to point to seconds next block
    first->next_block = second->next_block;

    //update previous of next block as long as it isnt null(check is necessary because our block might be the last block )
    if(second->next_block)
        second->next_block->prev_block = first;
}

//to request fresh new page to add to the front of the linked list O(1)
vm_page_t *
allocate_vm_page(vm_page_family_t *vm_page_family){

    //request fresh new page
    vm_page_t *vm_page = mm_get_new_vm_page_from_kernel(1);
   
    //Initialize lower most Meta block of the VM page
    MARK_VM_PAGE_EMPTY(vm_page);
    //offset is the extra space taken by the page family and doubly linked list of the page(not the data blocks)
    vm_page->block_meta_data.block_size =
        mm_max_page_allocatable_memory(1);
    vm_page->block_meta_data.offset =
        offset_of(vm_page_t, block_meta_data);
    init_glthread(&vm_page->block_meta_data.priority_thread_glue);
    vm_page->next = NULL;
    vm_page->prev = NULL;

    //Set the back pointer to page family
    vm_page->pg_family = vm_page_family;

    /*If it is a first VM data page for a given
     * page family*/
    if(!vm_page_family->first_page){
        vm_page_family->first_page = vm_page;
        return vm_page;
    }

    /* Insert new VM page to the head of the linked 
     * list*/
    vm_page->next = vm_page_family->first_page;
    vm_page_family->first_page->prev = vm_page;
    vm_page_family->first_page = vm_page;
    return vm_page;
}

//delete and free virtual memory page, and after it has been detached from doubly linked list and head is updated
void
mm_vm_page_delete_and_free(
        vm_page_t *vm_page){

    vm_page_family_t *vm_page_family =
        vm_page->pg_family;

    /*If the page being deleted is the head of the linked 
     * list*/
    if(vm_page_family->first_page == vm_page){
        vm_page_family->first_page = vm_page->next;
        if(vm_page->next)
            vm_page->next->prev = NULL;
        vm_page->next = NULL;
        vm_page->prev = NULL;
        mm_return_vm_page_to_kernel((void *)vm_page, 1);
        return;
    }

    /*If we are deleting the page from middle or end of 
     * linked list*/
    if(vm_page->next)
        vm_page->next->prev = vm_page->prev;
    vm_page->prev->next = vm_page->next;
    mm_return_vm_page_to_kernel((void *)vm_page, 1);
}

//to print the virtual memory details
void
mm_print_vm_page_details(vm_page_t *vm_page){

    printf("\t\t next = %p, prev = %p\n", vm_page->next, vm_page->prev);
    printf("\t\t page family = %s\n", vm_page->pg_family->struct_name);

    uint32_t j = 0;
    block_meta_data_t *curr;
    ITERATE_VM_PAGE_ALL_BLOCKS_BEGIN(vm_page, curr){

        printf("\t\t\t%-14p Block %-3u %s  block_size = %-6u  "
                "offset = %-6u  prev = %-14p  next = %p\n",
                curr,
                j++, curr->is_free ? "F R E E D" : "ALLOCATED",
                curr->block_size, curr->offset,
                curr->prev_block,
                curr->next_block);
    } ITERATE_VM_PAGE_ALL_BLOCKS_END(vm_page, curr);
}



//takes the struct name and size as input to make a page family
void
mm_instantiate_new_page_family(
    char *struct_name,
    uint32_t struct_size){


    //points to the most recent page(initially null)
    vm_page_family_t *vm_page_family_curr = NULL;
    vm_page_for_families_t *new_vm_page_for_families = NULL;

    //struct size shouldnt exceed size of system page as this requires 2 or mor contiguous page
    if(struct_size > SYSTEM_PAGE_SIZE){
        
        printf("Error : %s() Structure %s Size exceeds system page size\n",
            __FUNCTION__, struct_name);
        return;
    }

    //if there is no first page allocatted, allocate it, if it can be alocatted in already existing page, store it in that page otherwise get a new page and store it in the new page also update the linkedlist and make the head as the new page
    if(!first_vm_page_for_families){

        //requesting first page
        first_vm_page_for_families = 
            (vm_page_for_families_t *)mm_get_new_vm_page_from_kernel(1);
        first_vm_page_for_families->next = NULL;
        strncpy(first_vm_page_for_families->vm_page_family[0].struct_name, 
        struct_name, MM_MAX_STRUCT_NAME);
        first_vm_page_for_families->vm_page_family[0].struct_size = struct_size;
        first_vm_page_for_families->vm_page_family[0].first_page = NULL;
        init_glthread(&first_vm_page_for_families->vm_page_family[0].free_block_priority_list_head);
        return;
    }

	vm_page_family_curr = lookup_page_family_by_name(struct_name);

	if(vm_page_family_curr) {
		assert(0);
	}

    //to iterate through the pages, so that we can place the new struct if we already have an existing page with space
    uint32_t count = 0;

    ITERATE_PAGE_FAMILIES_BEGIN(first_vm_page_for_families, vm_page_family_curr){

	    count++;

    } ITERATE_PAGE_FAMILIES_END(first_vm_page_for_families, vm_page_family_curr);

    //if the current pages are already full, request a new page
    if(count == MAX_FAMILIES_PER_VM_PAGE){

        new_vm_page_for_families = 
            (vm_page_for_families_t *)mm_get_new_vm_page_from_kernel(1);
        new_vm_page_for_families->next = first_vm_page_for_families;
        first_vm_page_for_families = new_vm_page_for_families;
    }

    //count tells where page should be located
    strncpy(vm_page_family_curr->struct_name, struct_name,
            MM_MAX_STRUCT_NAME);
    vm_page_family_curr->struct_size = struct_size;
    vm_page_family_curr->first_page = NULL;
    init_glthread(&vm_page_family_curr->free_block_priority_list_head);
}

//to print the registered pages detals
void
mm_print_registered_page_families(){

    vm_page_family_t *vm_page_family_curr = NULL;
    vm_page_for_families_t *vm_page_for_families_curr = NULL;

    for(vm_page_for_families_curr = first_vm_page_for_families;
            vm_page_for_families_curr;
            vm_page_for_families_curr = vm_page_for_families_curr->next){

        ITERATE_PAGE_FAMILIES_BEGIN(vm_page_for_families_curr,
                vm_page_family_curr){

            printf("Page Family : %s, Size = %u\n",
                    vm_page_family_curr->struct_name,
                    vm_page_family_curr->struct_size);

        } ITERATE_PAGE_FAMILIES_END(vm_page_for_families_curr,
                vm_page_family_curr);
    }
}

//to compare for worst fit
static int
free_blocks_comparison_function(
        void *_block_meta_data1,
        void *_block_meta_data2){

    block_meta_data_t *block_meta_data1 =
        (block_meta_data_t *)_block_meta_data1;

    block_meta_data_t *block_meta_data2 =
        (block_meta_data_t *)_block_meta_data2;

    if(block_meta_data1->block_size > block_meta_data2->block_size)
        return -1;
    else if(block_meta_data1->block_size < block_meta_data2->block_size)
        return 1;
    return 0;
}

static void
mm_add_free_block_meta_data_to_free_block_list(
        vm_page_family_t *vm_page_family,
        block_meta_data_t *free_block){

    assert(free_block->is_free == MM_TRUE);
    glthread_priority_insert(&vm_page_family->free_block_priority_list_head,
            &free_block->priority_thread_glue,
            free_blocks_comparison_function,
            offset_of(block_meta_data_t, priority_thread_glue));
}

static vm_page_t *
mm_family_new_page_add(vm_page_family_t *vm_page_family){

    vm_page_t *vm_page = allocate_vm_page(vm_page_family);

    if(!vm_page)
        return NULL;

    /* The new page is like one free block, add it to the
     * free block list*/
    mm_add_free_block_meta_data_to_free_block_list(
            vm_page_family, &vm_page->block_meta_data);

    return vm_page;
}

/* Fn to mark block_meta_data as being Allocated for
 * 'size' bytes of application data. Return TRUE if 
 * block allocation succeeds*/
//3 argumnets: pointer to the base family, pointer to meta block of free data block, size of memory requested by application

//internal fragmentation is handled
//internal fragmentation case1: after allocatting memory, some free memory exists which cant be allocatted to the structure(soft internal fragmentation)- no handling during free
//case2: there is no space for a meta block if we assign a data block(hard internal fragmentation)
static vm_bool_t
mm_split_free_data_block_for_allocation(
            vm_page_family_t *vm_page_family,
            block_meta_data_t *block_meta_data, 
            uint32_t size){

    block_meta_data_t *next_block_meta_data = NULL;

    //only if its free allocate it
    assert(block_meta_data->is_free == MM_TRUE);

    //if data block isnt good nough return false
    if(block_meta_data->block_size < size){
        return MM_FALSE;
    }

    //size thats remaining after giving out a data block
    uint32_t remaining_size =
        block_meta_data->block_size - size;

    block_meta_data->is_free = MM_FALSE;
    block_meta_data->block_size = size;
    //once its allocatte dremove from priority queue
    remove_glthread(&block_meta_data->priority_thread_glue);
    /*block_meta_data->offset =  ??*/

    //entire free block is used, no residual memory
    //Case 1 : No Split
    if(!remaining_size){
        return MM_TRUE;
    }

    //no more space after allocatting
    /*Case 2 : Partial Split : Soft Internal Fragmentation*/
    else if(sizeof(block_meta_data_t) < remaining_size &&
            remaining_size < (sizeof(block_meta_data_t) + vm_page_family->struct_size)){
        /*New Meta block is to be created*/
        //initialise the new meta block
        next_block_meta_data = NEXT_META_BLOCK_BY_SIZE(block_meta_data);
        next_block_meta_data->is_free = MM_TRUE;
        next_block_meta_data->block_size =
            remaining_size - sizeof(block_meta_data_t);

        //calculate the offset of the metablock, and also append the metablock to the priority queue
        next_block_meta_data->offset = block_meta_data->offset +
            sizeof(block_meta_data_t) + block_meta_data->block_size;
        init_glthread(&next_block_meta_data->priority_thread_glue);
        mm_add_free_block_meta_data_to_free_block_list(
                vm_page_family, next_block_meta_data);
        //fixes all the linkage problem
        mm_bind_blocks_for_allocation(block_meta_data, next_block_meta_data);
    }
   
    //hard internal fragmentation
    /*Case 2 : Partial Split : Hard Internal Fragmentation*/
    else if(remaining_size < sizeof(block_meta_data_t)){
        /*No need to do anything !!*/
        //the linkages between metablocksare same as before
    }

    /*Case 3 : Full Split  : New Meta block is Created*/
    else {
        /*New Meta block is to be created*/
        next_block_meta_data = NEXT_META_BLOCK_BY_SIZE(block_meta_data);
        next_block_meta_data->is_free = MM_TRUE;
        next_block_meta_data->block_size =
            remaining_size - sizeof(block_meta_data_t);
        next_block_meta_data->offset = block_meta_data->offset +
            sizeof(block_meta_data_t) + block_meta_data->block_size;
        init_glthread(&next_block_meta_data->priority_thread_glue);
        mm_add_free_block_meta_data_to_free_block_list(
                vm_page_family, next_block_meta_data);
        mm_bind_blocks_for_allocation(block_meta_data, next_block_meta_data);
    }

    return MM_TRUE;

}

void mm_print_memory_usage_stats(vm_page_family_t *vm_page_family) {
    uint32_t total_memory_allocated = 0;
    uint32_t total_memory_freed = 0;
    uint32_t total_memory_in_use = 0;
    uint32_t total_blocks_allocated = 0;
    uint32_t total_blocks_freed = 0;
    uint32_t total_blocks_in_use = 0;
    // Iterate over all pages in the page family
    for (vm_page_t *vm_page = vm_page_family->first_page; vm_page != NULL; vm_page = vm_page->next) {
    // Calculate the address of the first block in the page
    block_meta_data_t *block = &vm_page->block_meta_data;
        // Calculate the address of the end of the page
    block_meta_data_t *end = (block_meta_data_t *)((char *)vm_page + SYSTEM_PAGE_SIZE);

    // Iterate over all blocks in the page
    while (block < end) {
        total_memory_allocated += block->block_size;
        total_blocks_allocated++;

        if (block->is_free) {
            total_memory_freed += block->block_size;
            total_blocks_freed++;
        }

        // Calculate the address of the next block
        block = (block_meta_data_t *)((char *)block + sizeof(block_meta_data_t) + block->block_size);
    }
}
    total_memory_in_use = total_memory_allocated - total_memory_freed;
    total_blocks_in_use = total_blocks_allocated - total_blocks_freed;

    printf("Total memory allocated: %u\n", total_memory_allocated);
    printf("Total memory freed: %u\n", total_memory_freed);
    printf("Total memory in use: %u\n", total_memory_in_use);
    printf("Total blocks allocated: %u\n", total_blocks_allocated);
    printf("Total blocks freed: %u\n", total_blocks_freed);
    printf("Total blocks in use: %u\n", total_blocks_in_use);
    printf("\n");
}

//allocate free data block for use by the application, returns the starting address of the metablock which guards the data block
static block_meta_data_t *
mm_allocate_free_data_block(
        vm_page_family_t *vm_page_family,
        uint32_t req_size){
    
    vm_bool_t status = MM_FALSE;
    vm_page_t *vm_page = NULL;
    block_meta_data_t *block_meta_data = NULL;

    block_meta_data_t *biggest_block_meta_data =
        mm_get_biggest_free_block_page_family(vm_page_family);

    //if theres no free block or existing free block isnt enough
    if(!biggest_block_meta_data ||
            biggest_block_meta_data->block_size < req_size){

        //Time to add a new page to Page family to satisfy the request
        vm_page = mm_family_new_page_add(vm_page_family);

        //Allocate the free block from this page now, splits the data block to allocate it
        status = mm_split_free_data_block_for_allocation(vm_page_family,
                &vm_page->block_meta_data, req_size);

        if(status){
            mm_print_memory_usage_stats(vm_page_family);
            return &vm_page->block_meta_data;
        }

        return NULL;
    }
    //The biggest block meta data can satisfy the request, this is the metablock we want to allocate
    if(biggest_block_meta_data){
        status = mm_split_free_data_block_for_allocation(vm_page_family,
                biggest_block_meta_data, req_size);
    }

    if(status){
        mm_print_memory_usage_stats(vm_page_family);
        return biggest_block_meta_data;
    }

    return NULL;
}

vm_page_family_t *
lookup_page_family_by_name(char *struct_name){

    vm_page_family_t *vm_page_family_curr = NULL;
    vm_page_for_families_t *vm_page_for_families_curr = NULL;

    for(vm_page_for_families_curr = first_vm_page_for_families;
            vm_page_for_families_curr;
            vm_page_for_families_curr = vm_page_for_families_curr->next){

        ITERATE_PAGE_FAMILIES_BEGIN(vm_page_for_families_curr, vm_page_family_curr){

            if(strncmp(vm_page_family_curr->struct_name,
                        struct_name,
                        MM_MAX_STRUCT_NAME) == 0){

                return vm_page_family_curr;
            }
        } ITERATE_PAGE_FAMILIES_END(vm_page_for_families_curr, vm_page_family_curr);
    }
    return NULL;
}

// In your memory management system
void mm_check_for_leaks() {
    int leak_detected = 0; // Flag to track if a leak is detected
        // Traverse the linked list of allocated blocks
    for (Allocation *block = head; block != NULL; block = block->next) {
        // If the block was not freed, print a warning message
        if (!block->freed) {
            printf("Warning: Memory leak detected. Block of size %lu was not freed.\n", block->size);
            leak_detected = 1; // Set the flag to true
        }
    }

    // If no leak was detected, print a message
    if (!leak_detected) {
        printf("No memory leaks detected.\n");
    }
}

/* The public fn to be invoked by the application for Dynamic
 * Memory Allocations.*/
void *
xcalloc(char *struct_name, int units){

    //search for a page family corresponding to a structure 
     vm_page_family_t *pg_family =
             lookup_page_family_by_name(struct_name);

    //return null if page doesnt exist
     if(!pg_family){

         printf("Error : Structure %s not registered with Memory Manager\n",
                 struct_name);
         return NULL;
     }

    //check if memory which app wants can be given by the existing vm page
     if(units * pg_family->struct_size > MAX_PAGE_ALLOCATABLE_MEMORY(1)){

         printf("Error : Memory Requested Exceeds Page Size\n");
         return NULL;
     }
     
     //Find the page which can satisfy the request
     block_meta_data_t *free_block_meta_data = NULL;

    //allocate the free data block which was found
     free_block_meta_data = mm_allocate_free_data_block(
             pg_family, units * pg_family->struct_size);


     if(free_block_meta_data){
         memset((char *)(free_block_meta_data + 1), 0, 
         free_block_meta_data->block_size);
          // Record the allocation
        Allocation* alloc = (Allocation*)malloc(sizeof(Allocation));
        alloc->ptr = (void *)(free_block_meta_data + 1);
        alloc->size = free_block_meta_data->block_size;
        alloc->freed = 0; // Mark the block as not freed
        alloc->next = head;
        head = alloc;

         return  (void *)(free_block_meta_data + 1);
     }

     return NULL;
}

//argument: metablock to be freed address, returns meta block which should be formedafter all the merging
static block_meta_data_t *
mm_free_blocks(block_meta_data_t *to_be_free_block){

    block_meta_data_t *return_block = NULL;

    assert(to_be_free_block->is_free == MM_FALSE);
    //pointer to page where metablock resides
    vm_page_t *hosting_page =
        MM_GET_PAGE_FROM_META_BLOCK(to_be_free_block);

    //pointer to virtuial page family
    vm_page_family_t *vm_page_family = hosting_page->pg_family;

    return_block = to_be_free_block;

    //mark as free
    to_be_free_block->is_free = MM_TRUE;

    //obtaining address of next metablock
    block_meta_data_t *next_block = NEXT_META_BLOCK(to_be_free_block);

    /*Handling Hard IF memory*/
    if(next_block){
        /* Scenario 1 : When data block to be freed is not the last
         * upper most meta block in a VM data page*/
        to_be_free_block->block_size +=
            mm_get_hard_internal_memory_frag_size (to_be_free_block, next_block);
    }
    else {
        /* Scenario 2: Page Boundry condition*/
        /* Block being freed is the upper most free data block
         * in a VM data page, check of hard internal fragmented
         * memory and merge*/

        //end address of vm page
        char *end_address_of_vm_page = (char *)((char *)hosting_page + SYSTEM_PAGE_SIZE);
        //end address of free data block
        char *end_address_of_free_data_block =
            (char *)(to_be_free_block + 1) + to_be_free_block->block_size;

        //subtract the 2to get size of internal fragmented memory
        int internal_mem_fragmentation = (int)((unsigned long)end_address_of_vm_page -
                (unsigned long)end_address_of_free_data_block);

        //add internal fragmented memory to the now freed block
        to_be_free_block->block_size += internal_mem_fragmentation;
    }

    //Now perform Merging
    //next block is null and is free, union with current block
    if(next_block && next_block->is_free == MM_TRUE){
        /*Union two free blocks*/
        mm_union_free_blocks(to_be_free_block, next_block);
        return_block = to_be_free_block;
    }
    /*Check the previous block if it was free*/
    block_meta_data_t *prev_block = PREV_META_BLOCK(to_be_free_block);

    //prev block is present and prev  block is free
    if(prev_block && prev_block->is_free){
        mm_union_free_blocks(prev_block, to_be_free_block);
        return_block = prev_block;
    }

    //if vm page becomes completely empty, delete the vm page
    if(mm_is_vm_page_empty(hosting_page)){
        mm_vm_page_delete_and_free(hosting_page);
        return NULL;
    }

    //add the metablock to the priority queue to make it available for allocation in the future.
    mm_add_free_block_meta_data_to_free_block_list(
            hosting_page->pg_family, return_block);

    return return_block;
}


//argument: pointer to data block which must be dleted
void
xfree(void *app_data){

    //size of meta block=datablock starting address-metablock size
    block_meta_data_t *block_meta_data =
        (block_meta_data_t *)((char *)app_data - sizeof(block_meta_data_t));

    //it should be full if we want to delete it
    assert(block_meta_data->is_free == MM_FALSE);
    //to empty
    mm_free_blocks(block_meta_data);

        // Mark the allocation as freed in the list
    Allocation* current = head;
    while (current) {
        if (current->ptr == app_data) {
            // Mark the block as freed
            current->freed = 1;
            break;
        } else {
            current = current->next;
        }
    }
}
//if next and previous is null and is filled is false then only page is empty
vm_bool_t
mm_is_vm_page_empty(vm_page_t *vm_page){

    if(vm_page->block_meta_data.next_block == NULL &&
            vm_page->block_meta_data.prev_block == NULL &&
            vm_page->block_meta_data.is_free == MM_TRUE){

        return MM_TRUE;
    }
    return MM_FALSE;
}



//print meta block data, for each page family prints occupied free meta block, occupied metablock and total metablock(fbc+obc=tbc)

//appmemusage: (size of metablock=48), size of datablocks+their metablocks, free meta block isnt used in calculation
void
mm_print_block_usage(){

    vm_page_t *vm_page_curr;
    vm_page_family_t *vm_page_family_curr;
    block_meta_data_t *block_meta_data_curr;
    uint32_t total_block_count, free_block_count,
             occupied_block_count;
    uint32_t application_memory_usage;

    ITERATE_PAGE_FAMILIES_BEGIN(first_vm_page_for_families, vm_page_family_curr){

        total_block_count = 0;
        free_block_count = 0;
        application_memory_usage = 0;
        occupied_block_count = 0;
        ITERATE_VM_PAGE_BEGIN(vm_page_family_curr, vm_page_curr){

            ITERATE_VM_PAGE_ALL_BLOCKS_BEGIN(vm_page_curr, block_meta_data_curr){

                total_block_count++;

                /*Sanity Checks*/
                if(block_meta_data_curr->is_free == MM_FALSE){
                    assert(IS_GLTHREAD_LIST_EMPTY(&block_meta_data_curr->\
                                priority_thread_glue));
                }
                if(block_meta_data_curr->is_free == MM_TRUE){
                    assert(!IS_GLTHREAD_LIST_EMPTY(&block_meta_data_curr->\
                                priority_thread_glue));
                }

                if(block_meta_data_curr->is_free == MM_TRUE){
                    free_block_count++;
                }
                else{
                    application_memory_usage +=
                        block_meta_data_curr->block_size + \
                        sizeof(block_meta_data_t);
                    occupied_block_count++;
                }
            } ITERATE_VM_PAGE_ALL_BLOCKS_END(vm_page_curr, block_meta_data_curr);
        } ITERATE_VM_PAGE_END(vm_page_family_curr, vm_page_curr);

        printf("%-20s   TBC : %-4u    FBC : %-4u    OBC : %-4u AppMemUsage : %u\n",
                vm_page_family_curr->struct_name, total_block_count,
                free_block_count, occupied_block_count, application_memory_usage);

    } ITERATE_PAGE_FAMILIES_END(first_vm_page_for_families, vm_page_family_curr);
}

//iterates over all pge family, and for all page family it prints all the vm page meta blocks
void
mm_print_memory_usage(char *struct_name){

    uint32_t i = 0;
    vm_page_t *vm_page = NULL;
    vm_page_family_t *vm_page_family_curr;
    uint32_t number_of_struct_families = 0;
    uint32_t cumulative_vm_pages_claimed_from_kernel = 0;

    printf("\nPage Size = %zu Bytes\n", SYSTEM_PAGE_SIZE);

    ITERATE_PAGE_FAMILIES_BEGIN(first_vm_page_for_families, vm_page_family_curr){

        if(struct_name){
            if(strncmp(struct_name, vm_page_family_curr->struct_name,
                        strlen(vm_page_family_curr->struct_name))){
                continue;
            }
        }

        number_of_struct_families++;

        printf(ANSI_COLOR_GREEN "vm_page_family : %s, struct size = %u\n"
                ANSI_COLOR_RESET,
                vm_page_family_curr->struct_name,
                vm_page_family_curr->struct_size);
        i = 0;

        ITERATE_VM_PAGE_BEGIN(vm_page_family_curr, vm_page){

            cumulative_vm_pages_claimed_from_kernel++;
            mm_print_vm_page_details(vm_page);

        } ITERATE_VM_PAGE_END(vm_page_family_curr, vm_page);
        printf("\n");
    } ITERATE_PAGE_FAMILIES_END(first_vm_page_for_families, vm_page_family_curr);

    printf(ANSI_COLOR_MAGENTA "# Of VM Pages in Use : %u (%lu Bytes)\n" \
            ANSI_COLOR_RESET,
            cumulative_vm_pages_claimed_from_kernel,
            SYSTEM_PAGE_SIZE * cumulative_vm_pages_claimed_from_kernel);

    float memory_app_use_to_total_memory_ratio = 0.0;

    printf("Total Memory being used by Memory Manager = %lu Bytes\n",
            cumulative_vm_pages_claimed_from_kernel * SYSTEM_PAGE_SIZE);
}


