#ifndef _BLOCKING_QUEUE_H_
#define _BLOCKING_QUEUE_H_

#include <vector>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <thread>
#include <memory>
#include <mutex>
#include <functional>
#include <deque>
#include <condition_variable>

template<class T>
class blocking_queue
{
public:
	blocking_queue(size_t sz = 0x7FFFFFFF):_size_limit(sz), _closed(false), _max_size(0){}

    //with Filter on element(get specific element)
	// return: true if found
	//         false if writer is closed
    template<class FilterFunc>
    bool get(T &ret, FilterFunc filter)
    {
        std::unique_lock<std::mutex> lk(_m);

        typename std::deque<T>::iterator it;
        _cv.wait(lk,
            [this, filter, &it]
            {
                //mutex auto relocked
                //check from the First Input to meet FIFO requirement
                for(it = _q.begin(); it != _q.end(); ++it)
                    if(filter(*it)) return true;

                //if closed & not-found, then we will never found it in the future
                if(_closed) return true;

                return false;
            });

        if(_q.size() > _max_size) _max_size = _q.size();

        //nothing will found in the future
        if(it == _q.end() && _closed)  return false;

        ret = *it; //copy construct the return value
        _q.erase(it);//remove from deque

        if(_q.size() < _size_limit)
            _cv_notfull.notify_all();

        return true;
    }

    bool get(T &ret){ return get(ret, [](const T &){return true;}); }

    bool put(const T & obj, bool drop_on_overflow = false)
    {
        std::unique_lock<std::mutex> lk(_m);

        if(drop_on_overflow && (_q.size() >= _size_limit))
        	return false;

        _cv_notfull.wait(lk, [this]{
            return _q.size() <_size_limit;
        });

        _q.push_back(obj);
        _cv.notify_all();

        return true;
    }

    void close(void)
    {
        std::unique_lock<std::mutex> lk(_m);
        _closed = true;
        _cv.notify_all();
    }
    int size(void){
        std::unique_lock<std::mutex> lk(_m);
        return _q.size();
    }
	size_t size_limit(){return _size_limit;}
	size_t max_size(void){ return _max_size;}
private:
    const size_t                    _size_limit;
    std::deque<T>                   _q;
    std::mutex                      _m;
    std::condition_variable        _cv;
    std::condition_variable        _cv_notfull;
    size_t                         _max_size;
    bool                           _closed;
};

#endif

