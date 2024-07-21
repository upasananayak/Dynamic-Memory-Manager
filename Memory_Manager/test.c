#include<stdio.h>

typedef struct Allocation {
    void* ptr;
    size_t size;
    int freed; 
    struct Allocation* next;
} Allocation;

int main(){
printf("Size of Allocation is: %zu\n", sizeof(Allocation));
return 0;
}
