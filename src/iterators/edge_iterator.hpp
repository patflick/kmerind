/*
 * edge_iterator.hpp
 *
 *  Created on: Aug 4, 2015
 *      Author: yongchao
 */

#ifndef EDGE_ITERATOR_HPP_
#define EDGE_ITERATOR_HPP_

#include <iterator>
#include "utils/function_traits.hpp"
#include "common/alphabets.hpp"

namespace bliss
{

  namespace iterator
  {

    // careful with the use of enable_if.  evaluation should occur at function call time,
    //   i.e. class template params will be evaluated with no substitution.
    // instead, a function should declare a template parameter proxy for the class template parameter.
    //   then enable_if evaluates using the proxy.
    //  e.g. template< class c = C; typename std::enable_if<std::is_same<c, std::string>::value, int>::type x = 0>


    /**
     * @class   edge_iterator
     * @brief   given a k-mer position, retrieve its left and right bases, including the dummy bases at both ends
     * @details
     *
     */
  	template<typename IT, typename Alphabet = bliss::common::DNA16>
  	class edge_iterator : public ::std::iterator<::std::forward_iterator_tag, uint8_t>
  	{
      protected:

          // curr position
          IT _curr;
          //previous position
          IT _prev;
          //a position of distance k from the _curr on the right
          IT _right;

          /*data*/
          const IT _data_start;
          const IT _data_end;

        public:
          typedef edge_iterator<IT, Alphabet> self_type; /*define edge iterator type*/
          typedef uint8_t edge_type; //type to represent an edge

          // accessors
          IT& getBase()
          {
            return _curr;
          }

          //constructor
          edge_iterator(IT data_start, IT data_end, const uint32_t k)
            : _curr (data_start), _prev(data_end), _right(data_start), _data_start(data_start), _data_end(data_end)
          {
            /*compute the offset*/
            ::std::advance(_right, k);
          }
          edge_iterator(IT data_end)
            : _curr(data_end), _prev(data_end), _right(data_end), _data_start(data_end), _data_end(data_end)
          {

          }
          /// copy constructor
          edge_iterator(const self_type& Other)
            : _curr (Other._curr), _prev(Other._prev), _right(Other._right),
              _data_start(Other._data_start), _data_end(Other._data_end)
          {
            /*do nothing*/
          }


          /// copy assignment iterator
          self_type& operator=(const self_type& Other)
          {
            _curr = Other._curr;
            _prev = Other._prev;
            _right = Other._right;
            _data_start = Other._data_start;
            _data_end = Other._data_end;

            return *this;
          }

          /// increment to next matching element in base iterator
          self_type& operator++()
          {  // if _curr at end, subsequent calls should not move _curr.
             // on call, if not at end, need to move first then evaluate.
            if (_curr == _data_end){  // if at end, don't move it.
              return *this;
            }

            /*save the previous position*/
            _prev = _curr;

            /*move forward by 1*/
            ++_curr;

            /*ensure that _right does not exceed _end*/
            if(_right != _data_end){
              ++_right;
            }
            return *this;
          }

          /**
           * post increment.  make a copy then increment that.
           */
          self_type operator++(int)
          {
            self_type output(*this);
            this->operator++();
            return output;
          }

          /// compare 2 filter iterators
          inline bool operator==(const self_type& rhs)
          {
            return _curr == rhs._curr;
          }

          /// compare 2 filter iterators
          inline bool operator!=(const self_type& rhs)
          {
            return _curr != rhs._curr;
          }

          /// dereference operator. _curr is guaranteed to be valid
          inline edge_type operator*()
          {
            /*using four bits to represent an edge*/
            if(_prev != _data_end && _right != _data_end){
                /*internal k-mer node*/
                return (Alphabet::FROM_ASCII[*_prev] << 4) | Alphabet::FROM_ASCII[*_right];
            }else if(_prev == _data_end && _right != _data_end){  /*the left-most k-mer node*/
              return  Alphabet::FROM_ASCII[*_right];
            }else if(_prev != _data_end && _right == _data_end){  /*the rigth-most k-mer node*/
              return Alphabet::FROM_ASCII[*_prev] << 4;
            }

            /*if(_prev == _end && _right == _end)*/
            return 0;
          }
      };

  	template<typename IT>
  	using DNA16_edge_iterator = edge_iterator<IT, bliss::common::DNA16>;
    template<typename IT>
    using DNA_IUPAC_edge_iterator = edge_iterator<IT, bliss::common::DNA_IUPAC>;
    template<typename IT>
    using DNA_edge_iterator = edge_iterator<IT, bliss::common::DNA>;
    template<typename IT>
    using DNA5_edge_iterator = edge_iterator<IT, bliss::common::DNA5>;
    template<typename IT>
    using RNA_edge_iterator = edge_iterator<IT, bliss::common::RNA>;
    template<typename IT>
    using RNA5_edge_iterator = edge_iterator<IT, bliss::common::RNA5>;


  	/*EdgeType = short unsigned int*/
  	template<typename IT>
  	class edge_iterator<IT, bliss::common::ASCII>: public ::std::iterator<::std::forward_iterator_tag, uint16_t>
  	{
  	protected:

        // curr position
        IT _curr;
        //previous position
        IT _prev;
        //a position of distance k from the _curr on the right
        IT _right;

        /*data*/
        const IT _data_start;
        const IT _data_end;

      public:
        typedef edge_iterator<IT, bliss::common::ASCII> self_type;	/*define edge iterator type*/
        typedef uint16_t edge_type;	//type to represent an edge

        // accessors
        IT& getBase()
        {
          return _curr;
        }

        //constructor
        edge_iterator(IT data_start, IT data_end, const uint32_t k)
        	: _curr (data_start), _prev(data_end), _right(data_start), _data_start(data_start), _data_end(data_end)
        {
        	/*compute the offset*/
        	::std::advance(_right, k);
        }
        edge_iterator(IT data_end)
        	: _curr(data_end), _prev(data_end), _right(data_end), _data_start(data_end), _data_end(data_end)
        {

        }

        /// copy constructor
        edge_iterator(const self_type& Other)
        	: _curr (Other._curr), _prev(Other._prev), _right(Other._right),
        	  _data_start(Other._data_start), _data_end(Other._data_end)
        {
        	/*do nothing*/
        }


        /// copy assignment iterator
        self_type& operator=(const self_type& Other)
        {
          _curr = Other._curr;
          _prev = Other._prev;
          _right = Other._right;
          _data_start = Other._data_start;
          _data_end = Other._data_end;

          return *this;
        }

        /// increment to next matching element in base iterator
        self_type& operator++()
        {  // if _curr at end, subsequent calls should not move _curr.
           // on call, if not at end, need to move first then evaluate.
          if (_curr == _data_end){  // if at end, don'IT move it.
            return *this;
          }

          /*save the previous position*/
          _prev = _curr;

          /*move forward by 1*/
          ++_curr;

          /*ensure that _right does not exceed _end*/
          if(_right != _data_end){
        	  ++_right;
          }
          return *this;
        }

        /**
         * post increment.  make a copy then increment that.
         */
        self_type operator++(int)
        {
        	self_type output(*this);
          this->operator++();
          return output;
        }

        /// compare 2 filter iterators
        inline bool operator==(const self_type& rhs)
        {
          return _curr == rhs._curr;
        }

        /// compare 2 filter iterators
        inline bool operator!=(const self_type& rhs)
        {
          return _curr != rhs._curr;
        }

        /// dereference operator. _curr is guaranteed to be valid
        inline edge_type operator*()
        {
        	/*using 8 bits to represent an edge*/
        	if(_prev != _data_end && _right != _data_end){
            	/*internal k-mer node*/
            	return (*_prev << 8) | *_right;
        	}else if(_prev == _data_end && _right != _data_end){	/*the left-most k-mer node*/
        		return *_right & 0x0ff;
        	}else if(_prev != _data_end && _right == _data_end){	/*the rigth-most k-mer node*/
        		return *_prev << 8;
        	}

        	/*if(_prev == _end && _right == _end)*/
        	return 0;
        }
    };
  	template<typename IT>
  	using raw_edge_iterator = edge_iterator<IT, bliss::common::ASCII>;


  } // iterator
} // bliss



#endif /* EDGE_ITERATOR_HPP_ */