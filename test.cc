#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "lfmalloc.hpp"
#include <pthread.h>
#include <iostream>

TEST(active,ptr_mask){
	lockfree::active a;
	a.set_credit(61);
	for(int i=0; i>0; i=i<<1){
		a.set_ptr(i);
		EXPECT_EQ(a.ptr() , reinterpret_cast<void*>(i));
	}
	EXPECT_EQ(a.credit(), 61);
}
TEST(active,credit_mask){
	lockfree::active a;
	a.set_ptr(0xdeadbeef);
	for(int i=0;i<64;i++){
		a.set_credit(i);
		EXPECT_EQ(a.credit() , i);
	}
	EXPECT_EQ(a.ptr(), reinterpret_cast<void*>(0xdeadbeef));
}
TEST(active,cas_success){
	lockfree::active a(0LL),b,null(0LL);
	a.set_ptr(10);
	b.set_ptr(0xdeadbeef);
	null.set_ptr(10);
	EXPECT_TRUE(a.cas(null,b));
	EXPECT_EQ(a.ptr(), reinterpret_cast<void*>(0xdeadbeef));
}
TEST(active,cas_failure){
	lockfree::active a(0LL),b,null(0LL);
	a.set_ptr(0xdeadbeef);a.set_credit(18);
	b.set_ptr(0xcafe);
	null.set_ptr(0xdeadbeef);null.set_credit(1);
	EXPECT_FALSE(a.cas(null,b));
	EXPECT_EQ(a.ptr(), reinterpret_cast<void*>(0xdeadbeef));
}
TEST(anchor, avail){
	lockfree::anchor a;
	a.set_state(lockfree::anchor::PARTIAL);
	a.set_count(10);
	a.set_tag(0xdeadbeef);
	
	for(size_t i=1;i<(1<<10);i<<=1){
		a.set_avail(i);
		EXPECT_EQ(a.avail(), i);
	}
	EXPECT_EQ(a.state(), lockfree::anchor::PARTIAL);
	EXPECT_EQ(a.count(), 10U);
	EXPECT_EQ(a.tag(), 0xdeadbeef);
}
TEST(anchor, count){
	lockfree::anchor a;
	a.set_state(lockfree::anchor::PARTIAL);
	a.set_avail(10);
	a.set_tag(0xdeadbeef);
	for(size_t i=1;i<(1<<10);i<<=1){
		a.set_count(i);
		EXPECT_EQ(a.count(), i);
	}
	EXPECT_EQ(a.state(), lockfree::anchor::PARTIAL);
	EXPECT_EQ(a.avail(), 10U);
	EXPECT_EQ(a.tag(), 0xdeadbeef);
}

TEST(malloc, malloc_from_newSB){
	lockfree::allocator aloc;
	int* get = reinterpret_cast<int*>(aloc.malloc(4));
	*get = 2;
	//std::cout << sizeof(lockfree::allocator);
}

TEST(malloc, malloc_from_newSB_again){
	lockfree::allocator aloc;
	int* get1 = reinterpret_cast<int*>(aloc.malloc(4));
	*get1 = 2;
	int* get2 = reinterpret_cast<int*>(aloc.malloc(4));
	*get2 = 3;
	EXPECT_EQ(*get1 , 2);
	EXPECT_EQ(*get2 , 3);
	std::cout << "get1:" << get1 << " get2:" << get2;
}

TEST(malloc, malloc_from_newSB_more_again){
	lockfree::allocator aloc;
	int* get1 = reinterpret_cast<int*>(aloc.malloc(4));
	*get1 = 2;
	int* get2 = reinterpret_cast<int*>(aloc.malloc(4));
	*get2 = 3;
	int* get3 = reinterpret_cast<int*>(aloc.malloc(4));
	*get3 = 4;
	EXPECT_EQ(*get1 , 2);
	EXPECT_EQ(*get2 , 3);
	EXPECT_EQ(*get3 , 4);
	std::cout << "get1:" << get1
						<< " get2:" << get2
						<< " get3:" << get3 << std::endl;
}
