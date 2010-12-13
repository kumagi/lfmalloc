#ifndef LFMALLOC_HPP
#define LFMALLOC_HPP

#include <cstddef>
#include <stdint.h>
#include <assert.h>

#include <iostream>

#include "atomic.hpp"
#include "memory_order.hpp"
#include <boost/static_assert.hpp>
#include <boost/thread/locks.hpp>
#include <boost/thread/shared_mutex.hpp>
#include <boost/array.hpp>
#include <boost/thread/tss.hpp>

#include <vector>

namespace lockfree{
using boost::memory_order_relaxed;
using boost::memory_order_acquire;
using boost::memory_order_release;
using boost::memory_order_acq_rel;
using boost::memory_order_seq_cst;
using boost::memory_order_consume;

using boost::shared_mutex;

typedef boost::detail::atomic::atomic_x86_64<uint64_t> atomic_uint64_t;

struct descriptor;

class active{// fit in one atomic block
	// ptr:58 credit:6
	atomic_uint64_t field;
	enum {
		credit_mask = 0xfc00000000000000LL,
		ptr_mask = 0x000003ffffffffffLL,
		credit_offset = 58,
		ptr_offset = 0
	};
public:
	active():field(0){
		//BOOST_STATIC_ASSERT(sizeof(void*) == 64);
	}
	active(uint64_t num):field(num){}
	void set_ptr(uint64_t ptr){
		assert((ptr & ~ptr_mask) == 0);
		field.store((field.load(memory_order_relaxed) & ~ptr_mask)
								| ptr,memory_order_relaxed);
	}
	void set_credit(int credit){
		assert(credit < (1<<6));
		field.store((field.load(memory_order_relaxed) & ptr_mask)
								| (static_cast<uint64_t>(credit) << 58), memory_order_relaxed);
	}
	descriptor* ptr()const{
		return reinterpret_cast<descriptor*>
			(field.load(memory_order_relaxed) & ptr_mask);
	}
	int credit()const{
		return (field.load(memory_order_relaxed) & credit_mask) >> 58;
	}
	bool cas(active& expected, const active& swap_for){
		uint64_t tmp = expected.field.load(memory_order_relaxed);
		return field.compare_exchange_strong(tmp
			, swap_for.field.load(memory_order_relaxed)
			,memory_order_seq_cst,memory_order_acquire);
	}
};
inline
std::ostream& operator<<(std::ostream& ost, const active& act){
	ost << "ptr:"<< act.ptr()
			<< " credit:" << act.credit();
	return ost;
}

class anchor{// fit in one atomic block
	// avail:10 count 10 state:2 tag:42
	atomic_uint64_t field;
	enum {
		avail_mask = 0xffc0000000000000LL,
		count_mask = 0x003ff00000000000LL,
		state_mask = 0x00000c0000000000LL,
		tag_mask = 0x000003ffffffffffLL,
		avail_offset = 54,
		count_offset = 44,
		state_offset = 42,
		tag_offset = 0
	};
public:
	enum state_t{
		ACTIVE = 0,
		EMPTY = 1,
		FULL = 2,
		PARTIAL = 3
	};
	enum max{
		active_max = 1024,
		count_mac = 1024,
	};
	anchor(){
		//BOOST_STATIC_ASSERT(sizeof(void*) == 64);
	}
	void set_avail(const size_t avail){
		field.store((field.load(memory_order_relaxed) & ~avail_mask)
								| (static_cast<uint64_t>(avail) << avail_offset), memory_order_relaxed);
	}
	void set_count(const size_t count){
		field.store((field.load(memory_order_relaxed) & ~count_mask)
								| (static_cast<uint64_t>(count) << count_offset), memory_order_relaxed);
	}
	void set_state(const size_t state){
		field.store((field.load(memory_order_relaxed) & ~state_mask)
								| (static_cast<uint64_t>(state) << state_offset), memory_order_relaxed);
	}
	void set_tag(const size_t tag){
		field.store((field.load(memory_order_relaxed) & ~tag_mask)
								| (static_cast<uint64_t>(tag) << tag_offset), memory_order_relaxed);
	}
	size_t avail()const{
		return (field.load(memory_order_relaxed) & avail_mask) >> avail_offset;
	}
	size_t count()const{
		return (field.load(memory_order_relaxed) & count_mask) >> count_offset;
	}
	state_t state()const{
		return static_cast<state_t>
			((field.load(memory_order_relaxed) & state_mask) >> state_offset);
	}
	uint64_t tag()const{
		return (field.load(memory_order_relaxed) & tag_mask) >> tag_offset;
	}
	bool cas(anchor& expected, const anchor& swap_for){
		uint64_t expect = expected.field.load(memory_order_acquire);
		const uint64_t swap = swap_for.field.load(memory_order_acquire);
		return field.compare_exchange_strong
			(expect, swap, memory_order_seq_cst, memory_order_acquire);
	}
};
inline 
std::ostream& operator<<(std::ostream& ost, const anchor& anc){
	ost << "avail:" << anc.avail()
			<< " count:" << anc.count()
			<< " state:" << 
		(anc.state() == anchor::ACTIVE) ? "ACTIVE"
		: (anc.state() == anchor::FULL) ? "FULL"
		: (anc.state() == anchor::EMPTY) ? "EMPTY"
		: (anc.state() == anchor::PARTIAL) ? "PARTIAL" : "invalid";
//			<< " tag:" << anc.tag();
	return ost;
}

struct proc_heap;

template <int size>
struct super_block{
	enum { blocksize = 4<<size, desc_size = 8, blocks = 1024 };
	boost::array<char, (blocksize + desc_size) * blocks> buff;
	super_block(){
		for(size_t i=0;i<blocks;i++){
			int* const fwd = reinterpret_cast<int*>(block(i));
			*fwd = i;
		}
	}
	void* block(const int index){
		return (&buff[0] + (blocksize+desc_size) * index);
	}
	const void* block(const int index)const{
		return (&buff[0] + (blocksize+desc_size) * index);
	}
	void marking(const int index,descriptor* const desc){
		uint64_t* target = reinterpret_cast<uint64_t*>
			(block(index+1)) - 1;
		*target = reinterpret_cast<uint64_t>(desc);
	}
	char* get_ptr(){return &buff[0];}
	operator char*(){return &buff[0];}
	descriptor* desc(){return reinterpret_cast<descriptor*>(buff[size]);}
};

union super_block_ptr{
#define sb_define(x) super_block<x>* size##x
	uint64_t none;
	sb_define(0);sb_define(1);sb_define(2);sb_define(3);sb_define(4);
	sb_define(5);sb_define(6);sb_define(7);sb_define(8);sb_define(9);
#undef sb_define
	char* get_ptr(){return reinterpret_cast<char*>(&none);}
	void marking(const int size,const int index, descriptor* const desc){
#define mark(x) if(size == x){size##x->marking(index,desc); return;}
		mark(0);mark(1);mark(2);mark(3);mark(4);
		mark(5);mark(6);mark(7);mark(8);mark(9);
#undef mark
	}
#define blk(x) if(size == x){return size##x->block(index);}
	void* block(const int size,const int index){
		blk(0);blk(1);blk(2);blk(3);blk(4);
		blk(5);blk(6);blk(7);blk(8);blk(9);
		return NULL;
	}
	const void* block(const int size, const int index)const{
		blk(0);blk(1);blk(2);blk(3);blk(4);
		blk(5);blk(6);blk(7);blk(8);blk(9);
		return NULL;
	}
#undef blk
	super_block_ptr(void* ptr_):none(reinterpret_cast<uint64_t>(ptr_)){}
};

super_block_ptr sb_factory(int size){
#define factory(x) if(size == x){return new super_block<x>();}
	factory(0);factory(1);factory(2);factory(3);factory(4);
	factory(5);factory(6);factory(7);factory(8);factory(9);
#undef factory
}
void sb_delete(super_block_ptr ptr, int size){
	//if(size == 0) {super_block<0>* target = ptr.size0;delete target;}
#define del(x) if(size == x){super_block<x>* target = ptr.size##x; delete target;}
	del(0);del(1);del(2);del(3);del(4);
	del(5);del(6);del(7);del(8);del(9);
#undef del
}
std::ostream& operator<<(std::ostream& ost, const super_block_ptr& ptr){
	ost << ptr.size0;
	return ost;
}

struct descriptor{
	anchor anc;
	descriptor* next;
	super_block_ptr sb; // pointer to sperblock
	proc_heap* heap; // pointer to owner proc_heap
	size_t sz; // block size
	size_t maxcount; // superblock size/sz
	descriptor(const size_t size)
		:next(NULL),sb(NULL),heap(NULL),sz(size),maxcount(1024){}
	descriptor(const size_t size,super_block_ptr ptr, proc_heap* parent_heap)
		:next(NULL),sb(ptr),heap(parent_heap),sz(size),maxcount(1024){
		anc.set_avail(1);
	}
	~descriptor(){
		if(next != NULL) delete next;
		if(sb.get_ptr() != NULL){
			sb_delete(sb, sz);
		}
	}
};
inline 
std::ostream& operator<<(std::ostream& ost, const descriptor& desc){
	ost << "anchor:" << desc.anc
			<< " sb:" << desc.sb
			<< " maxcount:" << desc.maxcount << std::endl;
	return ost;
}

struct sizeclass;
struct proc_heap{
	active act; // initially NULL
	descriptor* partial; // initially NULL
	sizeclass* sc; // pointer to parent sizeclass
	//proc_heap():partial(NULL),sc(NULL){}
	explicit proc_heap(sizeclass* parent)
		:partial(NULL),sc(parent){std::cout << "proc_heap construct\n";}
	~proc_heap(){
		if(partial != NULL) delete partial;
	}
};
struct sizeclass{
	descriptor* partial; // initially empty list
	size_t sz; // block size
	size_t sbsize; // superblock size
	sizeclass(){}
	sizeclass(size_t sz_)
		:partial(NULL),sz(sz_),sbsize(sz_*1024){}
	~sizeclass(){
		if(partial != NULL) delete partial;
	}
};
inline
std::ostream& operator<<(std::ostream& ost, const sizeclass& sc){
	ost << "sizeclass{" << " size:" << sc.sz << "}";
	return ost;
}
inline
std::ostream& operator<<(std::ostream& ost, const proc_heap& heap){
	ost << "active[" << heap.act << "]"
			<< " partial:" << *heap.partial
			<< " sizeclass:" << *heap.sc << std::endl;;
	return ost;
}

class allocator{
	struct do_noting{
		do_noting(){}
		void operator()(proc_heap*){};
	};
	boost::array<boost::thread_specific_ptr<proc_heap>, 10> heaps;
	boost::array<sizeclass, 10> size_classes;
public:
	allocator(){
#define sc_define(x) size_classes[x] = sizeclass(4<<x);
		sc_define(0);sc_define(1);sc_define(2);sc_define(3);sc_define(4);
		sc_define(5);sc_define(6);sc_define(7);sc_define(8);sc_define(9);
#undef sc_define
	}
	void* malloc(const size_t size){
		if(size > (1 << 16)) {return malloc(size);}
		proc_heap& heap = find_heap(size);
		std::cout << "found heap: " << &heap.act << ":" << heap.act << std::endl;
		void* addr = NULL;
		while(1){
			addr = malloc_from_active(heap);
			if(addr) return addr;
			addr = malloc_from_partial(heap);
			if(addr) return addr;
			addr = malloc_from_newSB(heap);
			if(addr) return addr;
			printf("one more time \n");
		}
	}
	proc_heap& find_heap(const size_t size){
		int sclass=0;
		{// get sizeclass
			int localsize = std::max(size, 4U);
			while(localsize){localsize>>=1;sclass++;}
		}
		if(heaps[sclass].get() == NULL){
			heaps[sclass].reset(new proc_heap(&size_classes[sclass]));
		}
		return *heaps[sclass].get();
	}
private:
	void* malloc_from_active(proc_heap& heap){
		active newactive,oldactive;
		do{
			oldactive = newactive = heap.act;
			std::cout << &heap.act<< ":" << heap.act << std::endl;
			if(oldactive.ptr() == NULL){ std::cout << "non active\n"; return NULL;}
			if(oldactive.credit() == 0){
				newactive.set_ptr(NULL);
				newactive.set_credit(0);
			}else{
				newactive.set_credit(oldactive.credit() - 1);
			}
		}while(!heap.act.cas(oldactive,newactive));
		std::cout << "get block from active heap" << std::endl;
		descriptor* desc = oldactive.ptr();
		void* addr;
		anchor oldanchor, newanchor;
		int morecredits;
		do{
			// stpate may be active, partial or empty
			oldanchor = newanchor = desc->anc;
			addr = desc->sb.block(heap.sc->sz,oldanchor.avail());
			char next = *reinterpret_cast<uint8_t*>(addr);
			newanchor.set_avail(next);
			newanchor.set_tag(oldanchor.tag()+1);
			std::cout << "next is " << static_cast<int>(next) << std::endl;
			if(oldactive.credit() == 0){
				if(oldanchor.count() == 0){
					newanchor.set_state(anchor::FULL);
				}else{
					morecredits = std::min(oldanchor.count(),1024U);
					newanchor.set_count(oldanchor.count() - morecredits);
				}
			}
		}while(!desc->anc.cas(oldanchor,newanchor));
		if(oldactive.credit() == 0 && oldanchor.count()>0){
			update_active(heap,desc,morecredits);
		}
		return addr;
	}
	void* malloc_from_partial(proc_heap& heap){ return NULL;}
	void* malloc_from_newSB(proc_heap& heap){
		descriptor* const desc 
			= new descriptor(heap.sc->sz, sb_factory(heap.sc->sz), &heap);
		assert(heap.sc->sbsize/desc->sz == 1024);
		
		active newactive,nullactive(0LL);
		newactive.set_ptr(reinterpret_cast<uint64_t>(desc));
		newactive.set_credit(std::min(desc->maxcount-1,1U<<6)-1);
		desc->anc.set_count((desc->maxcount-1)-(newactive.credit()+1));
		desc->anc.set_state(anchor::ACTIVE);
		// memory fence here!
		std::cout << "heap from newsb" << std::endl;
		if(heap.act.cas(nullactive,newactive)){
			desc->sb.marking(heap.sc->sz, 0, desc);
			std::cout << "cased " << &heap.act << ":" << heap.act << std::endl;
			return desc->sb.get_ptr();
		}else{ // someone cased
			assert(!"arien!");
			sb_delete(desc->sb, heap.sc->sz);
			delete desc;
			return NULL;
		}
	}
	void update_active(proc_heap& heap, descriptor* desc, int more){}
	
	static void block_marking(void* block, size_t size, descriptor* desc){
		uint64_t* const target = reinterpret_cast<uint64_t*>
			(reinterpret_cast<char*>(block) + size);
		*target = reinterpret_cast<uint64_t>(desc);
	}
};
}
#endif
