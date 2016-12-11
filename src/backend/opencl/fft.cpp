/*******************************************************
 * Copyright (c) 2014, ArrayFire
 * All rights reserved.
 *
 * This file is distributed under 3-clause BSD license.
 * The complete license agreement can be obtained at:
 * http://arrayfire.com/licenses/BSD-3-Clause
 ********************************************************/

#include <af/dim4.hpp>
#include <Array.hpp>
#include <copy.hpp>
#include <fft.hpp>
#include <err_opencl.hpp>
#include <err_clfft.hpp>
#include <clFFT.h>
#include <math.hpp>
#include <string>
#include <deque>
#include <utility>
#include <cstdio>
#include <memory.hpp>

using af::dim4;
using std::string;

namespace opencl
{

typedef std::pair<string, clfftPlanHandle> FFTPlanPair;
typedef std::deque<FFTPlanPair> FFTPlanCache;

// clFFTPlanner caches fft plans
//
// new plan |--> IF number of plans cached is at limit, pop the least used entry and push new plan.
//          |
//          |--> ELSE just push the plan
// existing plan -> reuse a plan
class clFFTPlanner
{
    friend void find_clfft_plan(clfftPlanHandle &plan,
                                clfftLayout iLayout, clfftLayout oLayout,
                                clfftDim rank, size_t *clLengths,
                                size_t *istrides, size_t idist,
                                size_t *ostrides, size_t odist,
                                clfftPrecision precision, size_t batch);

    public:
        static clFFTPlanner& getInstance() {
            static clFFTPlanner instances[opencl::DeviceManager::MAX_DEVICES];
            return instances[opencl::getActiveDeviceId()];
        }

        ~clFFTPlanner() {
            //TODO: FIXME:
            // clfftTeardown() cause a "Pure Virtual Function Called" crash on
            // Window only when Intel devices are called. This causes tests to
            // fail.
            #ifndef OS_WIN
            static bool flag = true;
            if(flag) {
                // THOU SHALL NOT THROW IN DESTRUCTORS
                clfftTeardown();
                flag = false;
            }
            #endif
        }

        inline void setMaxCacheSize(size_t size) {
            mCache.resize(size, FFTPlanPair(std::string(""), 0));
        }

        inline size_t getMaxCacheSize() const {
            return mMaxCacheSize;
        }

        inline clfftPlanHandle getPlan(int index) const {
            return mCache[index].second;
        }

        // iterates through plan cache from front to back
        // of the cache(queue)
        int findIfPlanExists(std::string keyString) const {
            int retVal = -1;
            for(uint i=0; i<mCache.size(); ++i) {
                if (keyString == mCache[i].first) {
                    retVal = i;
                }
            }
            return retVal;
        }

        // pops plan from the back of cache(queue)
        void popPlan() {
            if (!mCache.empty()) {
                // destroy the clfft plan associated with the
                // least recently used plan
                CLFFT_CHECK(clfftDestroyPlan(&mCache.back().second));
                // now pop the entry from cache
                mCache.pop_back();
            }
        }

        // pushes plan to the front of cache(queue)
        void pushPlan(std::string keyString, clfftPlanHandle plan) {
            if (mCache.size()>mMaxCacheSize) {
                popPlan();
            }
            mCache.push_front(FFTPlanPair(keyString, plan));
        }

    private:
        clFFTPlanner() : mMaxCacheSize(5) {
            CLFFT_CHECK(clfftInitSetupData(&mFFTSetup));
            CLFFT_CHECK(clfftSetup(&mFFTSetup));
        }
        clFFTPlanner(clFFTPlanner const&);
        void operator=(clFFTPlanner const&);

        clfftSetupData  mFFTSetup;

        size_t       mMaxCacheSize;
        FFTPlanCache mCache;
};

void find_clfft_plan(clfftPlanHandle &plan,
                     clfftLayout iLayout, clfftLayout oLayout,
                     clfftDim rank, size_t *clLengths,
                     size_t *istrides, size_t idist,
                     size_t *ostrides, size_t odist,
                     clfftPrecision precision, size_t batch)
{
    // create the key string
    char key_str_temp[64];
    sprintf(key_str_temp, "%d:%d:%d:", iLayout, oLayout, rank);

    string key_string(key_str_temp);

    /* WARNING: DO NOT CHANGE sprintf format specifier */
    for(int r=0; r<rank; ++r) {
        sprintf(key_str_temp, SIZE_T_FRMT_SPECIFIER ":", clLengths[r]);
        key_string.append(std::string(key_str_temp));
    }

    if(istrides!=NULL) {
        for(int r=0; r<rank; ++r) {
            sprintf(key_str_temp, SIZE_T_FRMT_SPECIFIER ":", istrides[r]);
            key_string.append(std::string(key_str_temp));
        }
        sprintf(key_str_temp, SIZE_T_FRMT_SPECIFIER ":", idist);
        key_string.append(std::string(key_str_temp));
    }

    if (ostrides!=NULL) {
        for(int r=0; r<rank; ++r) {
            sprintf(key_str_temp, SIZE_T_FRMT_SPECIFIER ":", ostrides[r]);
            key_string.append(std::string(key_str_temp));
        }
        sprintf(key_str_temp, SIZE_T_FRMT_SPECIFIER ":", odist);
        key_string.append(std::string(key_str_temp));
    }

    sprintf(key_str_temp, "%d:" SIZE_T_FRMT_SPECIFIER, (int)precision, batch);
    key_string.append(std::string(key_str_temp));

    // find the matching plan_index in the array clFFTPlanner::mKeys
    clFFTPlanner &planner = clFFTPlanner::getInstance();

    int planIndex = planner.findIfPlanExists(key_string);

    // if found a valid plan, return it
    if (planIndex!=-1) {
        plan = planner.getPlan(planIndex);
        return;
    }

    clfftPlanHandle temp;

    // getContext() returns object of type Context
    // Context() returns the actual cl_context handle
    CLFFT_CHECK(clfftCreateDefaultPlan(&temp, getContext()(), rank, clLengths));

    // complex to complex
    if (iLayout == oLayout) {
        CLFFT_CHECK(clfftSetResultLocation(temp, CLFFT_INPLACE));
    } else {
        CLFFT_CHECK(clfftSetResultLocation(temp, CLFFT_OUTOFPLACE));
    }

    CLFFT_CHECK(clfftSetLayout(temp, iLayout, oLayout));
    CLFFT_CHECK(clfftSetPlanBatchSize(temp, batch));
    CLFFT_CHECK(clfftSetPlanDistance(temp, idist, odist));
    CLFFT_CHECK(clfftSetPlanInStride(temp, rank, istrides));
    CLFFT_CHECK(clfftSetPlanOutStride(temp, rank, ostrides));
    CLFFT_CHECK(clfftSetPlanPrecision(temp, precision));
    CLFFT_CHECK(clfftSetPlanScale(temp, CLFFT_BACKWARD, 1.0));

    // getQueue() returns object of type CommandQueue
    // CommandQueue() returns the actual cl_command_queue handle
    CLFFT_CHECK(clfftBakePlan(temp, 1, &(getQueue()()), NULL, NULL));

    plan = temp;

    // push the plan into plan cache
    planner.pushPlan(key_string, plan);
}

void setFFTPlanCacheSize(size_t numPlans)
{
    clFFTPlanner::getInstance().setMaxCacheSize(numPlans);
}

template<typename T> struct Precision;
template<> struct Precision<cfloat > { enum {type = CLFFT_SINGLE}; };
template<> struct Precision<cdouble> { enum {type = CLFFT_DOUBLE}; };

static void computeDims(size_t rdims[4], const dim4 &idims)
{
    for (int i = 0; i < 4; i++) {
        rdims[i] = (size_t)idims[i];
    }
}

//(currently) true is in clFFT if length is a power of 2,3,5
inline bool isSupLen(dim_t length)
{
    while( length > 1 )
    {
        if( length % 2 == 0 )
            length /= 2;
        else if( length % 3  == 0 )
            length /= 3;
        else if( length % 5  == 0 )
            length /= 5;
        else if( length % 7  == 0 )
            length /= 7;
        else if( length % 11 == 0 )
            length /= 11;
        else if( length % 13 == 0 )
            length /= 13;
        else
            return false;
    }
    return true;
}

template<int rank>
void verifySupported(const dim4 dims)
{
    for (int i = 0; i < rank; i++) {
        ARG_ASSERT(1, isSupLen(dims[i]));
    }
}

template<typename T, int rank, bool direction>
void fft_inplace(Array<T> &in)
{
    verifySupported<rank>(in.dims());
    size_t tdims[4], istrides[4];

    computeDims(tdims   , in.dims());
    computeDims(istrides, in.strides());

    clfftPlanHandle plan;

    int batch = 1;
    for (int i = rank; i < 4; i++) {
        batch *= tdims[i];
    }

    find_clfft_plan(plan,
                    CLFFT_COMPLEX_INTERLEAVED,
                    CLFFT_COMPLEX_INTERLEAVED,
                    (clfftDim)rank, tdims,
                    istrides, istrides[rank],
                    istrides, istrides[rank],
                    (clfftPrecision)Precision<T>::type,
                    batch);

    cl_mem imem = (*in.get())();
    cl_command_queue queue = getQueue()();

    CLFFT_CHECK(clfftEnqueueTransform(plan,
                                      direction ? CLFFT_FORWARD : CLFFT_BACKWARD,
                                      1, &queue, 0, NULL, NULL,
                                      &imem, &imem, NULL));
}

template<typename Tc, typename Tr, int rank>
Array<Tc> fft_r2c(const Array<Tr> &in)
{
    dim4 odims = in.dims();

    odims[0] = odims[0] / 2 + 1;

    Array<Tc> out = createEmptyArray<Tc>(odims);

    verifySupported<rank>(in.dims());
    size_t tdims[4], istrides[4], ostrides[4];

    computeDims(tdims   ,  in.dims());
    computeDims(istrides,  in.strides());
    computeDims(ostrides, out.strides());

    clfftPlanHandle plan;

    int batch = 1;
    for (int i = rank; i < 4; i++) {
        batch *= tdims[i];
    }

    find_clfft_plan(plan,
                    CLFFT_REAL,
                    CLFFT_HERMITIAN_INTERLEAVED,
                    (clfftDim)rank, tdims,
                    istrides, istrides[rank],
                    ostrides, ostrides[rank],
                    (clfftPrecision)Precision<Tc>::type,
                    batch);

    cl_mem imem = (*in.get())();
    cl_mem omem = (*out.get())();
    cl_command_queue queue = getQueue()();

    CLFFT_CHECK(clfftEnqueueTransform(plan,
                                      CLFFT_FORWARD,
                                      1, &queue, 0, NULL, NULL,
                                      &imem, &omem, NULL));

    return out;
}

template<typename Tr, typename Tc, int rank>
Array<Tr> fft_c2r(const Array<Tc> &in, const dim4 &odims)
{
    Array<Tr> out = createEmptyArray<Tr>(odims);

    verifySupported<rank>(odims);
    size_t tdims[4], istrides[4], ostrides[4];

    computeDims(tdims   ,  odims);
    computeDims(istrides,  in.strides());
    computeDims(ostrides, out.strides());

    clfftPlanHandle plan;

    int batch = 1;
    for (int i = rank; i < 4; i++) {
        batch *= tdims[i];
    }

    find_clfft_plan(plan,
                    CLFFT_HERMITIAN_INTERLEAVED,
                    CLFFT_REAL,
                    (clfftDim)rank, tdims,
                    istrides, istrides[rank],
                    ostrides, ostrides[rank],
                    (clfftPrecision)Precision<Tc>::type,
                    batch);

    cl_mem imem = (*in.get())();
    cl_mem omem = (*out.get())();
    cl_command_queue queue = getQueue()();

    CLFFT_CHECK(clfftEnqueueTransform(plan,
                                      CLFFT_BACKWARD,
                                      1, &queue, 0, NULL, NULL,
                                      &imem, &omem, NULL));

    return out;
}

#define INSTANTIATE(T)                                      \
    template void fft_inplace<T, 1, true >(Array<T> &in);   \
    template void fft_inplace<T, 2, true >(Array<T> &in);   \
    template void fft_inplace<T, 3, true >(Array<T> &in);   \
    template void fft_inplace<T, 1, false>(Array<T> &in);   \
    template void fft_inplace<T, 2, false>(Array<T> &in);   \
    template void fft_inplace<T, 3, false>(Array<T> &in);   \

    INSTANTIATE(cfloat )
    INSTANTIATE(cdouble)

#define INSTANTIATE_REAL(Tr, Tc)                                        \
    template Array<Tc> fft_r2c<Tc, Tr, 1>(const Array<Tr> &in);         \
    template Array<Tc> fft_r2c<Tc, Tr, 2>(const Array<Tr> &in);         \
    template Array<Tc> fft_r2c<Tc, Tr, 3>(const Array<Tr> &in);         \
    template Array<Tr> fft_c2r<Tr, Tc, 1>(const Array<Tc> &in, const dim4 &odims); \
    template Array<Tr> fft_c2r<Tr, Tc, 2>(const Array<Tc> &in, const dim4 &odims); \
    template Array<Tr> fft_c2r<Tr, Tc, 3>(const Array<Tc> &in, const dim4 &odims); \

    INSTANTIATE_REAL(float , cfloat )
    INSTANTIATE_REAL(double, cdouble)
}
