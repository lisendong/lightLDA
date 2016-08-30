/*!
 * \file alias_table.h
 * \brief Defines alias table
 */

#ifndef LIGHTLDA_ALIAS_TABLE_H_
#define LIGHTLDA_ALIAS_TABLE_H_

#include <memory>
#include <mutex>
#include <vector>

#if defined(_WIN32) || defined(_WIN64)
// vs currently not support c++11 keyword thread_local
#define _THREAD_LOCAL __declspec(thread) 
#else 
#define _THREAD_LOCAL thread_local 
#endif

namespace multiverso { namespace lightlda
{
    class ModelBase;
    class xorshift_rng;
    class AliasTableIndex;

    /*!
     * \brief AliasTable is the storage for alias tables used for fast sampling
     *  from lightlda word proposal distribution. It optimize memory usage 
     *  through a hybrid storage by exploiting the sparsity of word proposal.
     *  AliasTable containes two part: 1) a memory pool to store the alias
     *  2) an index table to access each row
     */
    class AliasTable
    {
    public:
        AliasTable();
        ~AliasTable();
        /*!
         * \brief Set the table index. Must call this method before 
         */
        void Init(AliasTableIndex* table_index);
        /*!
         * \brief Build alias table for a word
         * \param word word to bulid
         * \param model access
         * \return success of not
         */
        int Build(int word, ModelBase* model);
        /*!
         * \brief sample from word proposal distribution
         * \param word word to sample
         * \param rng random number generator
         * \return sample proposed from the distribution
         */
        int Propose(int word, xorshift_rng& rng);
        /*!
         * \brief using N_k(summary_row table) to init asymmetric alpha's alias table
         * \param model use the summary_row table in it
         */
        void InitAsymmetricAlpha(ModelBase* model);
        /*!
         * \brief sample from asymmetric alphas
         * \param rng random number generator
         */
        int32_t ProposeAsymmetricAlpha(xorshift_rng& rng) const ;
        /*! \brief get the asymmetric alpha value */
        float AlphaAt(int32_t topic_id) const {
            // return the topic's current alpha value
            return alphas_[topic_id];
        }
        /*! \brief get the asymmetric alpha value sum */
        float AsyAlphaSum() const {
            return asy_alpha_sum_;
        }        

        /*! \brief Clear the alias table */
        void Clear();
    private:
        void AliasMultinomialRNG(int32_t size, float mass, int32_t& height,
            int32_t* kv_vector);
        int* memory_block_;
        int64_t memory_size_;
        AliasTableIndex* table_index_;

        std::vector<int32_t> height_;
        std::vector<float> mass_;
        // store alpha for each topic, update when topic summary row updates, size = TopicNumber
        std::vector<float> alphas_;
        int32_t beta_height_;
        // used for asymmetric alpha alias table 
        int32_t alpha_height_;
        float beta_mass_;
        

        int32_t* beta_kv_vector_;
        int32_t* alpha_kv_vector_;

        // thread local storage used for building alias
        _THREAD_LOCAL static std::vector<float>* q_w_proportion_;
        _THREAD_LOCAL static std::vector<int>* q_w_proportion_int_;
        _THREAD_LOCAL static std::vector<std::pair<int, int>>* L_;
        _THREAD_LOCAL static std::vector<std::pair<int, int>>* H_;

        int num_vocabs_;
        int num_topics_;
        float alpha_;
        float beta_;
        float asymmetric_alpha_;
        float asy_alpha_sum_;
        float beta_sum_;

        // No copying allowed
        AliasTable(const AliasTable&);
        void operator=(const AliasTable&);
    };
} // namespace lightlda
} // namespace multiverso
#endif // LIGHTLDA_ALIAS_TABLE_H_
