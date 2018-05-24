#include "types.h"
#include "stat.h"
#include "user.h"
#include "param.h"
#include <math.h>

#define PGSIZE      4096

void miniTest2(int pagesAmount, int shouldSuccess);
void runTests();

/*
* Allocates more than 16 pages to a process, which should invoke swap-outs
* from ram to swapfile, and then frees them.
*/
void test1(void){

	int testNum = 1;
	printf(2, "TEST %d:\n", testNum);

	int pagesAmount = 20;
    void* mallocs[pagesAmount];

    int test1pid = fork();
    if (test1pid == 0){
	    
	    printf(2, "Starting to allocate %d pages..\n",pagesAmount, PGSIZE);
	    for (int i=0; i<pagesAmount; i++){
	    	printf(2,"Iteration num: %d\n", i+1);
	   		mallocs[i] = (void*)malloc(PGSIZE);
	    }
	    printf(2, "Starting to free pages..\n");
	    for (int i=0; i<pagesAmount; i++){ 
	   		free(mallocs[i]);		
	    }
	    exit();
	}
	
	wait();

	printf(2, "TEST %d PASSED!\n\n", testNum);
}

/*
* Checks memory allocating limits. Attempts of allocating too many pages
* should cause a failure. (1 - should success, 0 - should NOT success)
*/
void test2(void){

	int testNum = 2;
	printf(1, "TEST %d:\n", testNum);

	miniTest2(20, 1);
	miniTest2(21, 1);
	miniTest2(22, 1);
	miniTest2(25, 1);

	printf(2, "TEST %d PASSED!\n\n", testNum);
}

void miniTest2(int pagesAmount, int shouldSuccess){

	void* malloced;
    int test1pid = fork();
    if (test1pid == 0){ //child
	    printf(2, "going to malloc %d pages..\n", pagesAmount);
  		malloced = (void*)malloc(PGSIZE*pagesAmount);
	    
	    if(shouldSuccess){
			if(malloced == 0)
				printf(2, "TEST FAILED!\n");
			else
				printf(2, "TEST PASSED!\n");
	    }
	    else{
	    	if(malloced == 0)
				printf(2, "TEST PASSED!\n");
			else
				printf(2, "TEST FAILED!\n");
	    }
	    
	    if(malloced != 0)
	    	free(malloced);

	    exit();
	}
	
	wait(); //clean all the pages of test1pid
}

/*
* Allocates more than 16 pages to a process, which should invoke swap-outs
* from ram to swapfile, and then trying to access thoese pages. At last, frees them.
*/
void test3(){

	int testNum = 3;
	printf(1, "TEST %d:\n", testNum);

	int memSize = 80000;
    printf(2, "Allocating space in memory for %d bytes (%d pages)..\n", memSize, memSize/PGSIZE + 1);
    char * malloced = malloc(memSize);
    for (int i = 0; i < memSize; i++) {
        malloced[i] = '@';
    }

    free(malloced);
	printf(2, "TEST %d PASSED!\n\n", testNum);
}


/*
* Checks swapfile content inheritance, by creating more then 16 pages which should invoke swap-outs
* from ram to swapfile, and signing the pages with a unique number. Afterward, the test checks
* that the above pages were inherited successfully to a child process using those numbers.
* Also, it checks that child's page modifications does not effect fathers pages.
*/
void test4(){

	int testNum = 4;
	printf(1, "TEST %d:\n", testNum);


	int pagesAmount = 17;
    // char mallocs[pagesAmount][PGSIZE];
    int memSize = pagesAmount*PGSIZE;
    char* mallocs;
    mallocs = malloc(memSize);
	
    for (int i=0; i < pagesAmount; i++){
   		mallocs[i*PGSIZE] = i;
    }

	if(fork() == 0){
		
	    for (int i=0; i < pagesAmount; i++){
			
	    	if(mallocs[i*PGSIZE] != i){
	    		printf(1, "FAILED!\n");
	    		break;
	    	}
	    	else
				printf(1, "SON PASSED!\n");

	   		mallocs[i*PGSIZE] = i*10;
	    }

	    free(mallocs);
	    exit();
	}

	wait();

	for (int i=0; i < pagesAmount; i++){
		
		if(mallocs[i*PGSIZE] != i){
			printf(1, "FAILED!\n");
			break;
		}
		else
			printf(1, "FATHER PASSED!\n");

	}

	free(mallocs);

	printf(2, "TEST %d PASSED!\n\n", testNum);
}


void TEST(void (*test)(void)){
	if(fork() == 0){
		test();
		exit();
	}

	wait();
}

void runTestsNTimes(int n){

	for(int i=0; i < n; i++){
		printf(1, "***ITERATION %d***\n", i+1);
		runTests();
	}
}

void runTests(){
	
	if(fork() == 0){

		TEST(test1);
		TEST(test2);
		TEST(test3);
		TEST(test4);

		exit();
	}

	wait();
}

int
main(int argc, char *argv[]){
    printf(1, "Starting myMem Tests:\n");
	
	runTests();

	exit();
}



