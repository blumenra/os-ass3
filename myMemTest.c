#include "types.h"
#include "stat.h"
#include "user.h"
#include "param.h"

#define PGSIZE      4096

void miniTest2(int pagesAmount, int shouldSuccess);
//actually malloc first allocates some pages (i saw 3) and then use it.

void test1(void){

	int pagesAmount = 20;
	printf(2, "myMem Tests:\n");
    void* arr[pagesAmount];
    int test1pid = fork();
    if (test1pid == 0){ //child
	    printf(2, "TEST 1:\n");
	    printf(2, "test1: going to malloc with %d*%d nbytes.\n",pagesAmount, PGSIZE);
	    for (int k=0; k<pagesAmount; k++){
	    	printf(2,"%d: MALLOC ITERATION\n", k+1);
	   		arr[k] = (void*)malloc(PGSIZE);
	    }
	    printf(2, "going to free.\n");
	    for (int k=0; k<pagesAmount; k++){ 
	   		free(arr[k]);		
	    }
	    exit();
	}
	else 
		wait(); //clean all the pages of test1pid

    printf(2, "FINISH TEST 1\n");    
    printf(2, "Father (pid: %d) exit.\n", getpid());
    exit();

}

void test2(void){

	int testNum = 2;
	printf(2, "TEST %d:\n", testNum);
	// miniTest2(20, 1);
	// miniTest2(21, 1);
	// miniTest2(22, 1);
	// miniTest2(28, 1);
	miniTest2(30, 1);
	// miniTest2(31, 1);
	// miniTest2(32, 1);
	// miniTest2(33, 0);
	// miniTest2(34, 0);
    printf(2, "FINISH TEST %d\n", testNum);    
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
	    	
	    exit();
	}
	else 
		wait(); //clean all the pages of test1pid

    // printf(2, "Father (pid: %d) exit.\n", getpid());
    exit();
}

void TEST(void (*test)(void)){
	if(fork() == 0){
		test();
	}

	wait();
}

int
main(int argc, char *argv[]){
    
	TEST(test1);
	// TEST(test2);

    exit();
}



