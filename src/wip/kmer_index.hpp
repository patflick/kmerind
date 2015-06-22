/**
 * @file    kmer_index.hpp
 * @ingroup index
 * @author  Tony Pan <tpan7@gatech.edu>
 * @brief	High level kmer indexing API
 * @details	3 primary Kmer Index classes are currently provided:
 * 			Kmer Count Index,
 * 			Kmer Position Index, and
 * 			Kmer Position + Quality score index.
 *
 *
 *		Currently, KmerIndex classes only supports FASTQ files, but the intent is to
 *		  allow replaceable module for file reading.
 *
 *		Data distribution is via replaceable Hash functions.  currently,
 *		  PrefixHasher is used to distribute to MPI processes based on the first log_2(P) bits of the kmer
 *		  InfixHasher is used to distribute to threads within a process, based on the next log_2(p) bits of the kmer
 *		  SuffixHasher is used for threadlocal unordered_map hashing
 *
 *		All are deterministic to allow simple lookup processes.
 *
 *
 * Copyright (c) 2014 Georgia Institute of Technology.  All Rights Reserved.
 *
 * TODO add License
 */
#ifndef KMERINDEX2_HPP_
#define KMERINDEX2_HPP_

#if defined(USE_MPI)
#include "mpi.h"
#endif

//#if defined(USE_OPENMP)
//#include "omp.h"
//#endif

#include <unistd.h>     // sysconf
#include <sys/stat.h>   // block size.
#include <utility>
#include <type_traits>

#include "io/fastq_loader.hpp"
//#include "io/fasta_loader.hpp"
//#include "io/fasta_iterator.hpp"

#include "utils/logging.h"
#include "common/alphabets.hpp"
#include "common/kmer.hpp"
#include "common/base_types.hpp"
#include "utils/kmer_utils.hpp"

#include "io/mxx_support.hpp"
#include "wip/distributed_unordered_map.hpp"
#include "wip/distributed_unordered_map_vec.hpp"
#include "wip/distributed_sorted_map.hpp"
#include "wip/distributed_map.hpp"

#include "io/sequence_iterator.hpp"
#include "io/sequence_id_iterator.hpp"
#include "iterators/transform_iterator.hpp"
#include "common/kmer_iterators.hpp"
#include "iterators/zip_iterator.hpp"
#include "index/quality_score_iterator.hpp"
#include "wip/kmer_hash.hpp"

#include "utils/timer.hpp"

namespace bliss
{
  namespace index
  {
    namespace kmer
    {

      template <typename MapType>
      class Index {
        protected:
          MapType map;

          using KmerType = typename MapType::key_type;
          using IdType   = typename MapType::mapped_type;
          using TupleType =      std::pair<KmerType, IdType>;

          using Alphabet = typename KmerType::KmerAlphabet;

          using FileLoaderType = bliss::io::FASTQLoader<CharType, true, false>; // raw data type :  use CharType

          //====  now process the file, one L1 block (block partition by MPI Rank) at a time
          // from FileLoader type, get the block iter type and range type
          using FileBlockIterType = typename FileLoaderType::L1BlockType::iterator;

          using ParserType = bliss::io::FASTQParser<FileBlockIterType, void>;
          using SeqType = typename ParserType::SequenceType;
          using SeqIterType = bliss::io::SequencesIterator<ParserType>;


          /// converter from ascii to alphabet values
          using BaseCharIterator = bliss::iterator::transform_iterator<typename SeqType::IteratorType, bliss::common::ASCII2<Alphabet> >;

          /// kmer generation iterator
          using KmerIterType = bliss::common::KmerGenerationIterator<BaseCharIterator, KmerType>;


          MPI_Comm comm;
          int commSize;
          int commRank;

        public:
          Index(MPI_Comm _comm, int _comm_size) : map(_comm, _comm_size), comm(_comm) {
            MPI_Comm_size(_comm, &commSize);
            MPI_Comm_rank(_comm, &commRank);
          }

          virtual ~Index() {};

          MapType & get_map() {
            return map;
          }


          std::vector<KmerType> read_file_for_kmers(const std::string & filename, MPI_Comm comm) {

            std::vector< KmerType > result;
            TIMER_INIT(file);

            {


              TIMER_START(file);
              //==== create file Loader
              FileLoaderType loader(comm, filename, 1, sysconf(_SC_PAGE_SIZE));  // this handle is alive through the entire building process.
              typename FileLoaderType::L1BlockType partition = loader.getNextL1Block();
              TIMER_END(file, "open", partition.getRange().size());


              //== reserve
              TIMER_START(file);
              // modifying the local index directly here causes a thread safety issue, since callback thread is already running.
              // index reserve internally sends a message to itself.
              // call after getting first L1Block to ensure that file is loaded.
              size_t est_size = (loader.getKmerCountEstimate(KmerType::size) + commSize - 1) / commSize;
              result.reserve(est_size);
              TIMER_END(file, "reserve", est_size * sizeof(KmerType));



              // == create kmer iterator
              //            kmer_iter start(data, range);
              //            kmer_iter end(range.second,range.second);

              TIMER_START(file);

              ParserType parser;
              //=== copy into array
              while (partition.getRange().size() > 0) {
                //== process the chunk of data
                SeqType read;

                //==  and wrap the chunk inside an iterator that emits Reads.
                SeqIterType seqs_start(parser, partition.begin(), partition.end(), partition.getRange().start);
                SeqIterType seqs_end(partition.end());


                //== loop over the reads
                for (; seqs_start != seqs_end; ++seqs_start)
                {
                  // first get read
                  read = *seqs_start;

                  // then compute and store into index (this will generate kmers and insert into index)
                  if (read.seqBegin == read.seqEnd) continue;

                  //== set up the kmer generating iterators.
                  KmerIterType start(BaseCharIterator(read.seqBegin, bliss::common::ASCII2<Alphabet>()), true);
                  KmerIterType end(BaseCharIterator(read.seqEnd, bliss::common::ASCII2<Alphabet>()), false);

                  result.insert(result.end(), start, end);
                }

                partition = loader.getNextL1Block();
              }
              TIMER_END(file, "read", result.size());
            }

            TIMER_REPORT_MPI(file, commRank, comm);
            return result;
          }



          std::vector<TupleType> find(std::vector<KmerType> &query) {
            return map.find(query);
          }

          std::vector<std::pair<KmerType, size_t> > count(std::vector<KmerType> &query) {
            return map.count(query);
          }

          void erase(std::vector<KmerType> &query) {
            map.erase(query);
          }


          template <typename Predicate>
          std::vector<TupleType> find_if(std::vector<KmerType> &query, Predicate const &pred) {
            return map.find_if(query, pred);
          }
          template <typename Predicate>
          std::vector<TupleType> find_if(Predicate const &pred) {
            return map.find_if(pred);
          }

          template <typename Predicate>
          std::vector<std::pair<KmerType, size_t> > count_if(std::vector<KmerType> &query, Predicate const &pred) {
            return map.count_if(query, pred);
          }

          template <typename Predicate>
          std::vector<std::pair<KmerType, size_t> > count_if(Predicate const &pred) {
            return map.count_if(pred);
          }


          template <typename Predicate>
          void erase_if(std::vector<KmerType> &query, Predicate const &pred) {
            map.erase_if(query, pred);
          }

          template <typename Predicate>
          void erase_if(Predicate const &pred) {
            map.erase_if(pred);
          }

          size_t local_size() {
            return map.local_size();
          }

      };


      /*
       * TYPE DEFINITIONS
       */
      template <typename MapType>
      class PositionIndex : public Index<MapType > {
        protected:

          using Base = Index<MapType >;

          using KmerType = typename Base::KmerType;
          using IdType =   typename Base::IdType;
          using TupleType = typename Base::TupleType;

          using Alphabet = typename KmerType::KmerAlphabet;


          using FileLoaderType = bliss::io::FASTQLoader<CharType, true, false>; // raw data type :  use CharType

          //====  now process the file, one L1 block (block partition by MPI Rank) at a time
          // from FileLoader type, get the block iter type and range type
          using FileBlockIterType = typename FileLoaderType::L1BlockType::iterator;

          using ParserType = bliss::io::FASTQParser<FileBlockIterType, void>;
          using SeqType = typename ParserType::SequenceType;
          using SeqIterType = bliss::io::SequencesIterator<ParserType>;


          /// converter from ascii to alphabet values
          using BaseCharIterator = bliss::iterator::transform_iterator<typename SeqType::IteratorType, bliss::common::ASCII2<Alphabet> >;

          /// kmer generation iterator
          using KmerIterType = bliss::common::KmerGenerationIterator<BaseCharIterator, KmerType>;
          /// kmer position iterator type
          using IdIterType = bliss::iterator::SequenceIdIterator<IdType>;

          /// combine kmer iterator and position iterator to create an index iterator type.
          using KmerIndexIterType = bliss::iterator::ZipIterator<KmerIterType, IdIterType>;


        public:
          using map_type = MapType;

          PositionIndex(MPI_Comm _comm, int _comm_size) : Base(_comm, _comm_size) {};

          virtual ~PositionIndex() {};


          std::vector<TupleType> read_file(const std::string & filename, MPI_Comm comm) {
            std::vector< TupleType > temp;

            TIMER_INIT(file);

            {

              TIMER_START(file);
              //==== create file Loader
              FileLoaderType loader(comm, filename, 1, sysconf(_SC_PAGE_SIZE));  // this handle is alive through the entire building process.
              typename FileLoaderType::L1BlockType partition = loader.getNextL1Block();
              TIMER_END(file, "open", partition.getRange().size());

              //== reserve
              TIMER_START(file);
              // modifying the local index directly here causes a thread safety issue, since callback thread is already running.
              // index reserve internally sends a message to itself.
              // call after getting first L1Block to ensure that file is loaded.
              size_t est_size = (loader.getKmerCountEstimate(KmerType::size) + this->commSize - 1) / this->commSize;
              temp.reserve(est_size);

              TIMER_END(file, "reserve", est_size * sizeof(TupleType));

              // == create kmer iterator
              //            kmer_iter start(data, range);
              //            kmer_iter end(range.second,range.second);

              TIMER_START(file);

              ParserType parser;
              //=== copy into array
              while (partition.getRange().size() > 0) {
                //== process the chunk of data
                SeqType read;

                //==  and wrap the chunk inside an iterator that emits Reads.
                SeqIterType seqs_start(parser, partition.begin(), partition.end(), partition.getRange().start);
                SeqIterType seqs_end(partition.end());


                //== loop over the reads
                for (; seqs_start != seqs_end; ++seqs_start)
                {
                  // first get read
                  read = *seqs_start;

                  // then compute and store into index (this will generate kmers and insert into index)
                  if (read.seqBegin == read.seqEnd) continue;

                  //== set up the kmer generating iterators.
                  KmerIterType start(BaseCharIterator(read.seqBegin, bliss::common::ASCII2<Alphabet>()), true);
                  KmerIterType end(BaseCharIterator(read.seqEnd, bliss::common::ASCII2<Alphabet>()), false);

                  //== set up the position iterators
                  IdIterType id_start(read.id);
                  IdIterType id_end(read.id);

                  // ==== set up the zip iterators
                  KmerIndexIterType index_start(start, id_start);
                  KmerIndexIterType index_end(end, id_end);


                  temp.insert(temp.end(), index_start, index_end);
                }

                partition = loader.getNextL1Block();
              }

              TIMER_END(file, "read", temp.size());

            }
            TIMER_REPORT_MPI(file, this->commRank, this->comm);


            return temp;
          }



          void build(const std::string & filename, MPI_Comm comm) {
            // ==  open file
            //            distributed_file df;
            //            range = df.open(filename, communicator);  // memmap internally
            //            data = df.data();

            ::std::vector<TupleType> temp = this->read_file(filename, comm);

            build(temp);
          }

          void build(std::vector<TupleType> &temp) {
            TIMER_INIT(build);

            TIMER_START(build);
            this->map.reserve(temp.size());
            TIMER_END(build, "reserve", temp.size());


            // distribute
            TIMER_START(build);
            this->map.insert(temp);
            TIMER_END(build, "insert", this->map.local_size());


            TIMER_START(build);
            size_t m = this->map.update_multiplicity();
            TIMER_END(build, "multiplicity", m);

            TIMER_REPORT_MPI(build, this->commRank, this->comm);

          }


      };







      template <typename MapType>
      class PositionQualityIndex : public Index<MapType > {
        protected:
          using Base = Index<MapType>;

          using KmerType = typename Base::KmerType;
          using KmerInfoType =  typename Base::IdType;
          using TupleType = typename Base::TupleType;

          using Alphabet = typename KmerType::KmerAlphabet;
          using IdType = typename KmerInfoType::first_type;
          using QualType = typename KmerInfoType::second_type;

          using FileLoaderType = bliss::io::FASTQLoader<CharType, true, false>; // raw data type :  use CharType
          using FileBlockIterType = typename FileLoaderType::L1BlockType::iterator;

          //====  now process the file, one L1 block (block partition by MPI Rank) at a time

          using ParserType = bliss::io::FASTQParser<FileBlockIterType, QualType>;
          using SeqType = typename ParserType::SequenceType;
          using SeqIterType = bliss::io::SequencesIterator<ParserType>;


          /// converter from ascii to alphabet values
          using BaseCharIterator = bliss::iterator::transform_iterator<typename SeqType::IteratorType, bliss::common::ASCII2<Alphabet> >;

          /// kmer generation iterator
          using KmerIterType = bliss::common::KmerGenerationIterator<BaseCharIterator, KmerType>;
          /// kmer position iterator type
          using IdIterType = bliss::iterator::SequenceIdIterator<IdType>;

          using QualIterType = bliss::index::QualityScoreGenerationIterator<typename SeqType::IteratorType, 21, bliss::index::Illumina18QualityScoreCodec<QualType> >;

          /// combine kmer iterator and position iterator to create an index iterator type.
          using KmerInfoIterType = bliss::iterator::ZipIterator<IdIterType, QualIterType>;
          using KmerIndexIterType = bliss::iterator::ZipIterator<KmerIterType, KmerInfoIterType>;


        public:

          using map_type = MapType;

          PositionQualityIndex(MPI_Comm _comm, int _comm_size) : Base(_comm, _comm_size) {};

          virtual ~PositionQualityIndex() {};


          std::vector<TupleType> read_file(const std::string & filename, MPI_Comm comm) {
            std::vector< TupleType > temp;
            TIMER_INIT(file);

            {
              TIMER_START(file);
              //==== create file Loader
              FileLoaderType loader(comm, filename, 1, sysconf(_SC_PAGE_SIZE));  // this handle is alive through the entire building process.
              typename FileLoaderType::L1BlockType partition = loader.getNextL1Block();
              TIMER_END(file, "open", partition.getRange().size());

              //== reserve
              TIMER_START(file);
              // modifying the local index directly here causes a thread safety issue, since callback thread is already running.
              // index reserve internally sends a message to itself.
              // call after getting first L1Block to ensure that file is loaded.
              size_t est_size = (loader.getKmerCountEstimate(KmerType::size) + this->commSize - 1) / this->commSize;
              temp.reserve(est_size);

              TIMER_END(file, "reserve", est_size * sizeof(TupleType));


              // == create kmer iterator
              //            kmer_iter start(data, range);
              //            kmer_iter end(range.second,range.second);

              TIMER_START(file);

              ParserType parser;
              //=== copy into array
              while (partition.getRange().size() > 0) {
                //== process the chunk of data
                SeqType read;

                //==  and wrap the chunk inside an iterator that emits Reads.
                SeqIterType seqs_start(parser, partition.begin(), partition.end(), partition.getRange().start);
                SeqIterType seqs_end(partition.end());


                //== loop over the reads
                for (; seqs_start != seqs_end; ++seqs_start)
                {
                  // first get read
                  read = *seqs_start;

                  // then compute and store into index (this will generate kmers and insert into index)
                  if (read.seqBegin == read.seqEnd || read.qualBegin == read.qualEnd) continue;

                  //== set up the kmer generating iterators.
                  KmerIterType start(BaseCharIterator(read.seqBegin, bliss::common::ASCII2<Alphabet>()), true);
                  KmerIterType end(BaseCharIterator(read.seqEnd, bliss::common::ASCII2<Alphabet>()), false);

                  //== set up the position iterators
                  IdIterType id_start(read.id);
                  IdIterType id_end(read.id);

                  QualIterType qual_start(read.qualBegin);
                  QualIterType qual_end(read.qualEnd);

                  KmerInfoIterType info_start(id_start, qual_start);
                  KmerInfoIterType info_end(id_end, qual_end);


                  // ==== set up the zip iterators
                  KmerIndexIterType index_start(start, info_start);
                  KmerIndexIterType index_end(end, info_end);


                  temp.insert(temp.end(), index_start, index_end);
                  //        for (auto it = index_start; it != index_end; ++it) {
                  //          temp.push_back(*it);
                  //        }
                  //std::copy(index_start, index_end, temp.end());
                  //        INFOF("R %d inserted.  new temp size = %lu", commRank, temp.size());
                }

                partition = loader.getNextL1Block();
              }


              TIMER_END(file, "read", temp.size());


            }
            TIMER_REPORT_MPI(file, this->commRank, this->comm);


            return temp;
          }

          void build(const std::string & filename, MPI_Comm comm) {
            // ==  open file
            //            distributed_file df;
            //            range = df.open(filename, communicator);  // memmap internally
            //            data = df.data();

            ::std::vector<TupleType> temp = this->read_file(filename, comm);

            build(temp);
          }

          void build(std::vector<TupleType> &temp) {
            TIMER_INIT(build);

            TIMER_START(build);
            this->map.reserve(temp.size());
            TIMER_END(build, "reserve", temp.size());


            // distribute
            TIMER_START(build);
            this->map.insert(temp);
            TIMER_END(build, "insert", this->map.local_size());


            TIMER_START(build);
            size_t m = this->map.update_multiplicity();
            TIMER_END(build, "multiplicity", m);

            TIMER_REPORT_MPI(build, this->commRank, this->comm);

          }

      };





      template <typename MapType>
      class CountIndex : public Index<MapType> {
        protected:

          using Base = Index<MapType>;

          using KmerType = typename Base::KmerType;
          using IdType =   typename Base::IdType;
          using TupleType = typename Base::TupleType;


        public:
          using map_type = MapType;


          CountIndex(MPI_Comm _comm, int _comm_size) : Base(_comm, _comm_size) {};

          virtual ~CountIndex() {};

          std::vector<KmerType> read_file(const std::string & filename, MPI_Comm comm) {
            return this->read_file_for_kmers(filename, comm);
          }

          void build(const std::string & filename, MPI_Comm comm) {
            // ==  open file
            //            distributed_file df;
            //            range = df.open(filename, communicator);  // memmap internally
            //            data = df.data();

            ::std::vector<KmerType> temp = this->read_file(filename, comm);

            build(temp);
          }

          void build(std::vector<KmerType> &temp) {
            TIMER_INIT(build);

            TIMER_START(build);
            this->map.reserve(temp.size());
            TIMER_END(build, "reserve", temp.size());


            // distribute
            TIMER_START(build);
            this->map.insert(temp);
            TIMER_END(build, "insert", this->map.local_size());


            TIMER_START(build);
            size_t m = this->map.update_multiplicity();
            TIMER_END(build, "multiplicity", m);

            TIMER_REPORT_MPI(build, this->commRank, this->comm);

          }


      };

//      /**
//       * @brief    kmer position or pos+qual class template declaration
//       * @details  generic template for kmer index.  The following are user tunable:
//       *              K
//       *              Alphabet
//       *              file format:  also determines if quality score can be generated.
//       *              Map:          type of map providing storage.  use template template parameter.
//       *
//       *           following are params of the map providing the storage.
//       *           user should use templated type aliasing to hide these
//       *              communicator type: data movement mechanism.  template parameters of the functions
//       *              kmolecule_combiner:  none or xor or min - put kmolecule together
//       *              kmer transform : e.g. for minimim substring
//       *
//       *           following are determined based on user input:
//       *              WordType for kmer (depend on K)
//       *              KmerType
//       *
//       *           following are fixed:
//       *              farm hash  (for uniform bucketing locally), farm hash_prefix (uniform bucketing across processors/threads)
//       *              MapType:  depend on index type.  most tuple indices are going to be multimap.  aggregating index uses map.
//       *              ValueType:  depend on index type.
//       *
//       */
//      template<unsigned int KmerSize, typename Alphabet, template <typename, typename> class MapType >
//      class KmerIndex;
//
//      template<unsigned int KmerSize, typename Alphabet, template <typename, typename> class MapType >
//      class KmerIndex;
//
//
//
//      /// count index template declaration
//      template <typename KmerType, typename CountType, template <KmerType, CountType> class MapType,
//        typename std::enable_if<std::is_arithmetic<CountType>::value, int>::type = 0 >
//      class KmerCountIndex{
//        public:
//          /// constructor
//          KmerCountIndex() {};
//
//          /// destructor
//          virtual ~KmerCountIndex() {};
//
//          /// build
//          template <typename FileFormat, typename Communicator>
//          void build() {};
//
//          /// query
//          template <typename Communicator>
//          void query() {};
//      };
//
//      /// qual index specialization, only for FASTQ files
//      template <typename KmerType, template <KmerType, bliss::index::QualityType<bliss::io::FASTQ> > class MapType>
//      class KmerIndex<KmerType, bliss::index::QualityType<bliss::io::FASTQ>, MapType<KmerType, bliss::index::QualityType<bliss::io::FASTQ> > > {
//        public:
//          /// constructor
//          KmerIndex() {};
//
//          /// destructor
//          virtual ~KmerIndex() {};
//
//          /// build
//          template <typename FileFormat, typename Communicator>
//          void build() {};
//
//          /// query
//          template <typename Communicator>
//          void query() {};
//      };
//
//
//      /// position index specialization
//      template <typename FileFormat, typename KmerType, template <KmerType, typename FileFormat::SequenceId > class MapType>
//      class KmerIndex<KmerType, typename FileFormat::SequenceId, MapType<KmerType, typename FileFormat::SequenceId > > {
//
//        protected:
//          std::unordered_map<KmerType, typename FileFormat::SequenceId, bliss::hash::kmer::hash<KmerType, bliss::hash::kmer::detail::farm::hash,
//            ::bliss::hash::kmer::LexicographicLessCombiner, false> > map;
//
//        public:
//          /// constructor
//          KmerIndex() {};
//
//          /// destructor
//          virtual ~KmerIndex() {};
//
//          /// build.
//          template <typename FileFormat, typename Communicator>
//          void build(std::string filename, Communicator comm) {
//            // ==  open file
////            distributed_file df;
////            range = df.open(filename, communicator);  // memmap internally
////            data = df.data();
//
//            int commSize, commRank;
//            MPI_Comm_size(MPI_COMM_WORLD, &commSize);
//            MPI_Comm_rank(MPI_COMM_WORLD, &commRank);
//
//            {
//              std::chrono::high_resolution_clock::time_point t1, t2;
//              std::chrono::duration<double> time_span;
//
//              t1 = std::chrono::high_resolution_clock::now();
//              //==== create file Loader
//              FileLoaderType loader(MPI_COMM_WORLD, filename, 1, sysconf(_SC_PAGE_SIZE));  // this handle is alive through the entire building process.
//              typename FileLoaderType::L1BlockType partition = loader.getNextL1Block();
//              t2 = std::chrono::high_resolution_clock::now();
//               time_span =
//                   std::chrono::duration_cast<std::chrono::duration<double>>(
//                       t2 - t1);
//               INFOF("R %d file open time: %f", rank, time_span.count());
//
//               //== reserve
//               t1 = std::chrono::high_resolution_clock::now();
//              // modifying the local index directly here causes a thread safety issue, since callback thread is already running.
//              // index reserve internally sends a message to itself.
//               // call after getting first L1Block to ensure that file is loaded.
//               size_t est_size = (loader.getKmerCountEstimate(KmerType::size) + commSize - 1) / commSize;
//              map.reserve(est_size);
//              t2 = std::chrono::high_resolution_clock::now();
//               time_span =
//                   std::chrono::duration_cast<std::chrono::duration<double>>(
//                       t2 - t1);
//               INFOF("R %d reserve time: %f", rank, time_span.count());
//
//
//
//               // == create kmer iterator
//               //            kmer_iter start(data, range);
//               //            kmer_iter end(range.second,range.second);
//
//               t1 = std::chrono::high_resolution_clock::now();
//               std::vector<std::pair<KmerType, typename FileFormat::SequenceId > > temp(est_size);
//
//              //====  now process the file, one L1 block (block partition by MPI Rank) at a time
//               using FileLoaderType = bliss::io::FASTQLoader<CharType, true, false>; // raw data type :  use CharType
//               // from FileLoader type, get the block iter type and range type
//               using FileBlockIterType = typename FileLoaderType::L1BlockType::iterator;
//
//               using ParserType = bliss::io::FASTQParser<FileBlockIterType, void>;
//               using SeqType = typename ParserType::SequenceType;
//               ParserType parser;
//
//               using SeqIterType = bliss::io::SequencesIterator<ParserType>;
//
//
//               /// converter from ascii to alphabet values
//               using BaseCharIterator = bliss::iterator::transform_iterator<typename SeqType::IteratorType, bliss::common::ASCII2<Alphabet> >;
//
//               /// kmer generation iterator
//                       typedef bliss::common::KmerGenerationIterator<BaseCharIterator, KmerType>             KmerIterType;
//                       /// kmer position iterator type
//                               using IdIterType = bliss::iterator::SequenceIdIterator<IdType>;
//
//                               /// combine kmer iterator and position iterator to create an index iterator type.
//                                       using KmerIndexIterType = bliss::iterator::ZipIterator<KmerIterType, IdIterType>;
//
//              //=== copy into array
//              while (partition.getRange().size() > 0) {
//                //== process the chunk of data
//                SeqType read;
//
//                //==  and wrap the chunk inside an iterator that emits Reads.
//                SeqIterType seqs_start(parser, partition.begin(), partition.end(), partition.getRange().start);
//                SeqIterType seqs_end(partition.end());
//
//
//                //== loop over the reads
//                for (; seqs_start != seqs_end; ++seqs_start)
//                {
//                  // first get read
//                  read = *seqs_start;
//
//                  // then compute and store into index (this will generate kmers and insert into index)
//                  if (read.seqBegin == read.seqEnd) return;
//
//                  //== set up the kmer generating iterators.
//                  KmerIterType start(BaseCharIterator(read.seqBegin, bliss::common::ASCII2<Alphabet>()), true);
//                  KmerIterType end(BaseCharIterator(read.seqEnd, bliss::common::ASCII2<Alphabet>()), false);
//
//                  //== set up the position iterators
//                  IdIterType id_start(read.id);
//                  IdIterType id_end(read.id);
//
//                  // ==== set up the zip iterators
//                  KmerIndexIterType index_start(start, id_start);
//                  KmerIndexIterType index_end(end, id_end);
//
//                  std::copy(index_start, index_end, temp.end());
//
//                }
//
//                partition = loader.getNextL1Block();
//              }
//              t2 = std::chrono::high_resolution_clock::now();
//               time_span =
//                   std::chrono::duration_cast<std::chrono::duration<double>>(
//                       t2 - t1);
//               INFOF("R %d local kmer array time: %f", rank, time_span.count());
//
//
//               // distribute
//               t1 = std::chrono::high_resolution_clock::now();
//               using el_type = std::pair<KmerType, typename FileFormat::SequenceId >;
//
//               bliss::hash::kmer::hash<KmerType, bliss::hash::kmer::detail::farm::hash,::bliss::hash::kmer::LexicographicLessCombiner, true> hash(ceilLog2(commSize));
//
//               //bliss::hash::farm::KmerPrefixHash<KmerType> hash(ceilLog2(commSize));
//               mxx::msgs_all2all(temp, [&]( el_type const &x, el_type const &y){
//                 return (hash(x.first) % commSize) < (hash(y.first) % commSize);
//               }, MPI_COMM_WORLD);
//               t2 = std::chrono::high_resolution_clock::now();
//                time_span =
//                    std::chrono::duration_cast<std::chrono::duration<double>>(
//                        t2 - t1);
//                INFOF("R %d bucket and distribute: %f", rank, time_span.count());
//
//                // == insert into distributed map.
//                map.insert(temp.begin(), temp.end());
//
//
////            //distributed_map m(element_count);
////            m.reserve(element_count, communicator);
////            m.insert(start, end, communicator);
////            //m.local_rehash();
//
//            //df.close();
//
//            }
//          };
//
//          /// query.  specialize based on communicator type.
//          template <typename Communicator>
//          results query(kmers, Communicator comm) {
//
//            return m.equal_range(kmers);
//
//            // query from distributed map.
//
//          };
//      };
//
//      /// pos+qual index, only for FASTQ files
//      template <typename FileFormat, typename KmerType,
//                template <KmerType, std::pair<typename FileFormat::SequenceId, bliss::index::QualityType<FileFormat> > > class MapType>
//      class KmerIndex<KmerType, std::pair<typename FileFormat::SequenceId, bliss::index::QualityType<FileFormat> >,
//                      MapType<KmerType, std::pair<typename FileFormat::SequenceId, bliss::index::QualityType<FileFormat>> > > {
//        public:
//          /// constructor
//          KmerIndex() {};
//
//          /// destructor
//          virtual ~KmerIndex() {};
//
//          /// build
//          template <typename FileFormat, typename Communicator>
//          void build() {};
//
//          /// query
//          template <typename Communicator>
//          void query() {};
//      };


    } /* namespace kmer */

  } /* namespace index */
} /* namespace bliss */

#endif /* KMERINDEX2_HPP_ */
