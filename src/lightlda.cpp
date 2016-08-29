﻿#include "common.h"
#include "trainer.h"
#include "alias_table.h"
#include "data_stream.h"
#include "data_block.h"
#include "document.h"
#include "meta.h"
#include "util.h"
#include <vector>
#include <iostream>
#include <multiverso/barrier.h>
#include <multiverso/log.h>
#include <multiverso/row.h>

namespace multiverso { namespace lightlda
{     
    class LightLDA
    {
    public:
        static void Run(int argc, char** argv)
        {
            Config::Init(argc, argv);
            
            AliasTable* alias_table = new AliasTable();
            Barrier* barrier = new Barrier(Config::num_local_workers);
            meta.Init();
            std::vector<TrainerBase*> trainers;
	    // trainer 只是本地的线程数！！
            for (int32_t i = 0; i < Config::num_local_workers; ++i)
            {
                Trainer* trainer = new Trainer(alias_table, barrier, &meta);
                trainers.push_back(trainer);
            }

            ParamLoader* param_loader = new ParamLoader();
            multiverso::Config config;
            config.num_servers = Config::num_servers;
            config.num_aggregator = Config::num_aggregator;
            config.server_endpoint_file = Config::server_file;

            // param_loader 只 load 那个 slice 所需要的 table 下来到本机
            Multiverso::Init(trainers, param_loader, config, &argc, &argv);

            Log::ResetLogFile("LightLDA."
                + std::to_string(clock()) + ".log");

            data_stream = CreateDataStream();
            InitMultiverso();
            Train();
	    // DumpModel(trainers);

            Multiverso::Close();
            
            for (auto& trainer : trainers)
            {
                delete trainer;
            }
            delete param_loader;
            
            DumpDocTopic();

            delete data_stream;
            delete barrier;
            delete alias_table;
        }
    private:
        static void Train()
        {
            Multiverso::BeginTrain();
            for (int32_t i = 0; i < Config::num_iterations; ++i)
            {
                Multiverso::BeginClock();
                // Train corpus block by block
                for (int32_t block = 0; block < Config::num_blocks; ++block)
                {
                    data_stream->BeforeDataAccess();
                    DataBlock& data_block = data_stream->CurrDataBlock();
                    data_block.set_meta(&meta.local_vocab(block));
                    int32_t num_slice = meta.local_vocab(block).num_slice();
                    std::vector<LDADataBlock> data(num_slice);
                    // Train datablock slice by slice
                    for (int32_t slice = 0; slice < num_slice; ++slice)
                    {
                        LDADataBlock* lda_block = &data[slice];
                        lda_block->set_data(&data_block);
                        lda_block->set_iteration(i);
                        lda_block->set_block(block);
                        lda_block->set_slice(slice);
			// Trainer::TrainIteration 调用点是这里？
                        Multiverso::PushDataBlock(lda_block);
                    }
                    Multiverso::Wait();
                    data_stream->EndDataAccess();
                }
                Multiverso::EndClock();
            }
            Multiverso::EndTrain();
        }

        static void InitMultiverso()
        {
            Multiverso::BeginConfig();
            CreateTable();
            ConfigTable();
            Initialize();
            Multiverso::EndConfig();
        }

        static void Initialize()
        {
            xorshift_rng rng;
            for (int32_t block = 0; block < Config::num_blocks; ++block)
            {
                data_stream->BeforeDataAccess();
                DataBlock& data_block = data_stream->CurrDataBlock();
                int32_t num_slice = meta.local_vocab(block).num_slice();
		Log::Info("block %d/%d, num_slice=%d, data_block_size=%d", block + 1, Config::num_blocks, num_slice, data_block.Size());
                for (int32_t i = 0; i < data_block.Size(); ++i)
                {
		    // 一个完整的 doc 一定在一个 data_block 中
                    Document* doc = data_block.GetOneDoc(i);
		    int32_t max_word_id = 0;
		    for (int32_t word_idx = 0; word_idx < doc->Size(); ++word_idx) {
		      if (doc->Word(word_idx) > max_word_id) max_word_id = doc->Word(word_idx);
		    }
		    int32_t doc_topic_id = max_word_id % Config::num_topics;
		    for (int32_t word_idx = 0; word_idx < doc->Size(); ++word_idx) {
                      // Init the latent variable
		      // 直接随机从 k 个 topics 中选取一个
                      if (!Config::warm_start)
                          doc->SetTopic(word_idx, doc_topic_id);
                      // Init the server table
		      // word_id 和 topic_id 都是从 0 开始
                      Multiverso::AddToServer<int32_t>(kWordTopicTable,
                          doc->Word(word_idx), doc->Topic(word_idx), 1);
                      Multiverso::AddToServer<int64_t>(kSummaryRow,
                          0, doc->Topic(word_idx), 1);
                    }
                }
                Multiverso::Flush();
                data_stream->EndDataAccess();
            }
        }

        static void DumpDocTopic()
        {
            Row<int32_t> doc_topic_counter(0, Format::Sparse, kMaxDocLength); 
            for (int32_t block = 0; block < Config::num_blocks; ++block)
            {
                std::ofstream fout("doc_topic." + std::to_string(block));
                data_stream->BeforeDataAccess();
                DataBlock& data_block = data_stream->CurrDataBlock();
                for (int i = 0; i < data_block.Size(); ++i)
                {
                    Document* doc = data_block.GetOneDoc(i);
                    doc_topic_counter.Clear();
                    doc->GetDocTopicVector(doc_topic_counter);
                    fout << i << " ";  // doc id
                    Row<int32_t>::iterator iter = doc_topic_counter.Iterator();
                    while (iter.HasNext())
                    {
                        fout << " " << iter.Key() << ":" << iter.Value();
                        iter.Next();
                    }
                    fout << std::endl;
                }
                data_stream->EndDataAccess();
            }
        }

	// static void DumpModel(std::vector<TrainerBase*>& trainers) {
	//     int32_t num_vocabs = Config::num_vocabs;
	//     TrainerBase* trainer = trainers[0];
	//     std::string out = "model.out";
	//     std::ofstream fout(out);
	//     for (int word = 0; word < num_vocabs; ++word) {
	//         Row<int32_t>& row = trainer->GetRow<int32_t>(kWordTopicTable, word);
	// 	Row<int32_t>::iterator iter = row.Iterator();
	// 	fout << word;
	// 	while (iter.HasNext()) {
	// 	    int32_t topic = iter.Key();
	// 	    int32_t count = iter.Value();
	// 	    fout << " " << topic << ":" << count;
	// 	    iter.Next();
	// 	}
	// 	fout << std::endl;
	//     }
	// }

        static void CreateTable()
        {
            int32_t num_vocabs = Config::num_vocabs;
            int32_t num_topics = Config::num_topics;
            Type int_type = Type::Int;
            Type longlong_type = Type::LongLong;
            multiverso::Format dense_format = multiverso::Format::Dense;
            // multiverso::Format sparse_format = multiverso::Format::Sparse;

            Multiverso::AddServerTable(kWordTopicTable, num_vocabs,
                num_topics, int_type, dense_format);
            Multiverso::AddCacheTable(kWordTopicTable, num_vocabs,
                num_topics, int_type, dense_format, Config::model_capacity);
            Multiverso::AddAggregatorTable(kWordTopicTable, num_vocabs,
                num_topics, int_type, dense_format, Config::delta_capacity);

            Multiverso::AddTable(kSummaryRow, 1, Config::num_topics,
                longlong_type, dense_format);
        }
        
        static void ConfigTable()
        {
            multiverso::Format dense_format = multiverso::Format::Dense;
            multiverso::Format sparse_format = multiverso::Format::Sparse;
            for (int32_t word = 0; word < Config::num_vocabs; ++word)
            {
                if (meta.tf(word) > 0)
                {
		    // kLoadFactor = 2
                    if (meta.tf(word) * kLoadFactor > Config::num_topics)
                    {
                        Multiverso::SetServerRow(kWordTopicTable,
                            word, dense_format, Config::num_topics);
                        Multiverso::SetCacheRow(kWordTopicTable,
                            word, dense_format, Config::num_topics);
                    }
                    else
                    {
                        Multiverso::SetServerRow(kWordTopicTable,
                            word, sparse_format, meta.tf(word) * kLoadFactor);
                        Multiverso::SetCacheRow(kWordTopicTable,
                            word, sparse_format, meta.tf(word) * kLoadFactor);
                    }
                }
                if (meta.local_tf(word) > 0)
                {
                    if (meta.local_tf(word) * 2 * kLoadFactor > Config::num_topics)
                        Multiverso::SetAggregatorRow(kWordTopicTable, 
                            word, dense_format, Config::num_topics);
                    else
                        Multiverso::SetAggregatorRow(kWordTopicTable, word, 
                            sparse_format, meta.local_tf(word) * 2 * kLoadFactor);
                }
            }
        }
    private:
        /*! \brief training data access */
        static IDataStream* data_stream;
        /*! \brief training data meta information */
        static Meta meta;
    };
    IDataStream* LightLDA::data_stream = nullptr;
    Meta LightLDA::meta;

} // namespace lightlda
} // namespace multiverso


int main(int argc, char** argv)
{
    multiverso::lightlda::LightLDA::Run(argc, argv);
    return 0;
}
