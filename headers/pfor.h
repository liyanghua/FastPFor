/**
 * This is code is released under the
 * Apache License Version 2.0 http://www.apache.org/licenses/.
 *
 * (c) Daniel Lemire, http://lemire.me/en/
 */

#ifndef PFOR_H_
#define PFOR_H_

#include "common.h"
#include "codecs.h"
#include "bitpacking.h"
#include "util.h"

using namespace std;

/**
 * This implements as best as possible the PFor scheme
 * from  Zukowski et al., Super-Scalar RAM-CPU Cache Compression.
 *
 *  Implemented by D. Lemire
 *
 * Small differences:
 *  1. We don't write the exception section is reverse order.
 *     It is written in forward order.
 *
 *  2.  Obviously, the code is specific to 32-bit integers whereas
 *      the original description allowed for different data types.
 *
 *  3. Because we assume pfor delta, we don't compute the base (that is
 *     the frame). Correspondingly, we don't sort a sample. Instead,
 *     we use a fast approach to identify the best number of bits
 *     based on the computation of the integer logarithm.
 *
 *  4. Though it is not clear how the sample is taken in the original
 *     paper, we consider a consecutive sample of up to 64K samples.
 *
 */
class PFor: public IntegerCODEC {
public:
    enum {
        BlockSizeInUnitsOfPackSize = 4,
        PACKSIZE = 32,
        BlockSize = BlockSizeInUnitsOfPackSize * PACKSIZE,
        blocksizeinbits = constexprbits(BlockSize)
    };
    // these are reusable buffers
    vector<uint32_t> codedcopy;
    vector<size_t> miss;
    typedef uint32_t DATATYPE;// this is so that our code looks more like the original paper

    PFor() :
        codedcopy(BlockSize), miss(BlockSize) {
    }
    // for delta coding, we don't use a base.
    static uint32_t determineBestBase(
            const DATATYPE * in, size_t size) {
        if (size == 0) return 0;
        const size_t defaultsamplesize = 64 * 1024;
        // the original paper describes sorting 
        // a sample, but this only makes sense if you 
        // are coding a frame of reference.
        size_t samplesize = size > defaultsamplesize ? defaultsamplesize : size;
        uint32_t freqs[33];
        for (uint32_t k = 0; k <= 32; ++k)
            freqs[k] = 0;
        // we choose the sample to be consecutive
        uint32_t rstart = size > samplesize ? ( rand() % (size - samplesize) ) : 0;
        for (uint32_t k = rstart; k < rstart + samplesize; ++k) {
            freqs[asmbits(in[k])]++;
        }
        uint32_t bestb = 32;
        uint32_t numberofexceptions = 0;
        double Erate = 0;
        double bestcost = 32;
        for (uint32_t b = bestb - 1; b < 32; --b) {
             numberofexceptions += freqs[b + 1];
             Erate = numberofexceptions * 1.0 / samplesize;
            /**
            * though this is not explicit in the original paper, you
            * need to somehow compensate for compulsory exceptions
            * when the chosen number of bits is small.
            *
            * We use their formula (3.1.5) to estimate actual number
            * of total exceptions, including compulsory exceptions.
            */
            if(numberofexceptions > 0) {
              double altErate = (Erate * 128 - 1) / (Erate * (1U<<b));
              if(altErate > Erate) Erate = altErate;
            }
            const double thiscost = b + Erate * 32;
            if (thiscost <= bestcost) {
                bestcost = thiscost;
                bestb = b;
            }
        } 
        return bestb;
    }

    // returns location of first exception or BlockSize if there is none
    size_t compressblockPFOR(const DATATYPE * __restrict__ in,
            uint32_t * __restrict__ outputbegin, const uint32_t b,
            DATATYPE * __restrict__ & exceptions) {
        if (b == 32) {
            for (size_t k = 0; k < BlockSize; ++k)
                *(outputbegin++) = *(in++) ;
            return BlockSize;
        }
        size_t exceptcounter = 0;
        const uint32_t maxgap = 1U << b;
        {
            vector<uint32_t>::iterator cci = codedcopy.begin();
            for (size_t k = 0; k < BlockSize; ++k, ++cci) {
                miss[exceptcounter] = k;
                exceptcounter += (in[k] >= maxgap);
            }

        }
        if (exceptcounter == 0) {
            packblock(in, outputbegin, b);
            return BlockSize;
        }
        codedcopy.assign(in,in+BlockSize);
        size_t firstexcept = miss[0];
        size_t prev = 0;
        *(exceptions++) = codedcopy[firstexcept];
        prev = firstexcept;
        if (maxgap < BlockSize) {
            for (size_t i = 1; i < exceptcounter; ++i) {
                size_t cur = miss[i];
                // they don't include this part, but it is required:
                while (cur > maxgap + prev) {
                    // compulsory exception
                    size_t compulcur = prev + maxgap;
                    *(exceptions++) = codedcopy[compulcur];
                    codedcopy[prev] = maxgap - 1;
                    prev = compulcur;
                }
                *(exceptions++) = codedcopy[cur];
                codedcopy[prev] = cur - prev - 1;
                prev = cur;
            }
        } else {
            for (size_t i = 1; i < exceptcounter; ++i) {
                size_t cur = miss[i];
                *(exceptions++) = codedcopy[cur];
                codedcopy[prev] = cur - prev - 1;
                prev = cur;
            }
        }
        packblock(&codedcopy[0], outputbegin, b);
        return firstexcept;
    }

    void packblock(const uint32_t * source, uint32_t * out, const uint32_t bit) {
        for (uint32_t j = 0; j != BlockSize; j += PACKSIZE) {
            fastpack(source + j, out, bit);
            out += bit;
        }
    }

    void unpackblock(const uint32_t * source, uint32_t * out,
            const uint32_t bit) {
        for (uint32_t j = 0; j != BlockSize; j += PACKSIZE) {
            fastunpack(source, out + j, bit);
            source += bit;
        }
    }

    void encodeArray(const uint32_t *in, const size_t len, uint32_t *out,
            size_t &nvalue) {
        *out++ = len;
        const uint32_t * const finalin(in + len);
        const uint32_t maxsize = (1U << (32 - blocksizeinbits - 1));
        size_t totalnvalue(1);
        for (size_t i = 0; i < len; i += maxsize) {
            size_t l = maxsize;
            if (i + maxsize > len) {
                l = len - i;
                assert(l <= maxsize);
            }
            size_t thisnvalue = nvalue - totalnvalue;
            assert(in + i + l <= finalin);
            __encodeArray(&in[i], l, out, thisnvalue);
            totalnvalue += thisnvalue;
            assert(totalnvalue <= nvalue);
            out += thisnvalue;
        }
        nvalue = totalnvalue;
    }
    const uint32_t * decodeArray(const uint32_t *in, const size_t len,
            uint32_t *out, size_t &nvalue) {
        nvalue = *in++;
        if (nvalue == 0)
            return in;
        const uint32_t * const initin = in;
        const uint32_t * const finalin = in + len;
        size_t totalnvalue(0);
        while (totalnvalue < nvalue) {
            size_t thisnvalue = nvalue - totalnvalue;
            const uint32_t * const befin(in);
            assert(finalin <= len + in);
            in = __decodeArray(in, finalin - in, out, thisnvalue);
            assert(in > befin);
            assert(in <= finalin);
            out += thisnvalue;
            totalnvalue += thisnvalue;
            assert(totalnvalue <= nvalue);
        }
        assert(in <= len + initin);
        assert(in <= finalin);
        nvalue = totalnvalue;
        return in;

    }

    void __encodeArray(const uint32_t *in, const size_t len, uint32_t *out,
            size_t &nvalue) {
        checkifdivisibleby(len, BlockSize);
        const uint32_t * const initout(out);
        vector<DATATYPE> exceptions;
        exceptions.reserve(len);// possibly unsafe but can be faster than resize
        DATATYPE * __restrict__ i = &exceptions[0];
        const uint32_t b = determineBestBase(in,len);
        *out++ = len;
        *out++ = b;
        for (size_t k = 0; k < len / BlockSize; ++k) {
            uint32_t * const headerout(out);
            ++out;
            size_t firstexcept = compressblockPFOR(in, out, b, i);
            out += (BlockSize * b) / 32;
            in += BlockSize;
            const uint32_t bitsforfirstexcept = blocksizeinbits;
            const uint32_t firstexceptmask = (1U << blocksizeinbits) - 1;
            const size_t exceptindex = i - &exceptions[0];
            *headerout = (firstexcept & firstexceptmask) | (exceptindex
                    << bitsforfirstexcept);
        }
        const size_t howmanyexcept = i - &exceptions[0];
        for (uint32_t t = 0; t < howmanyexcept; ++t)
            *out++ = exceptions[t];
        nvalue = out - initout;
    }

    const uint32_t * __decodeArray(const uint32_t *in, const size_t len,
            uint32_t *out, size_t &nvalue) {
        const uint32_t * const initin(in);
        nvalue = *in++;
        checkifdivisibleby(nvalue, BlockSize);
         const uint32_t b = *in++;
        const DATATYPE * __restrict__ except = in + nvalue * b / 32 + nvalue
                / BlockSize;
        const uint32_t bitsforfirstexcept = blocksizeinbits;
        const uint32_t firstexceptmask = (1U << blocksizeinbits) - 1;
        const DATATYPE * endexceptpointer = except;
        const DATATYPE * const initexcept(except);
        for (size_t k = 0; k < nvalue / BlockSize; ++k) {
            const uint32_t * const headerin(in);
            ++in;
            const uint32_t firstexcept = *headerin & firstexceptmask;
            const uint32_t exceptindex = *headerin >> bitsforfirstexcept;
            endexceptpointer = initexcept + exceptindex;
            uncompressblockPFOR(in, out, b, except, endexceptpointer,
                    firstexcept);
            in += (BlockSize * b) / 32;
            out += BlockSize;
        }
        assert(initin + len >= in);
        assert(initin + len >= endexceptpointer);
        return endexceptpointer;
    }
    void uncompressblockPFOR(
            const uint32_t * __restrict__ inputbegin,// points to the first packed word
            DATATYPE * __restrict__ outputbegin, const uint32_t b,
            const DATATYPE *__restrict__ & i, // i points to value of the first exception
            const DATATYPE * __restrict__ end_exception, size_t next_exception // points to the position of the first exception
            ) {
        unpackblock(inputbegin, reinterpret_cast<uint32_t *> (outputbegin), b); /* bit-unpack the values */
         for (size_t cur = next_exception; i != end_exception; cur
                = next_exception) {
            next_exception = cur + static_cast<size_t> (outputbegin[cur]) + 1;
            outputbegin[cur] = *(i++);
        }
    }

    virtual string name() const {
        ostringstream convert;
        convert << "PFor";
        return convert.str();
    }

};

#endif /* PFOR_H_ */
