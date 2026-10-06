/* Originally from JAERO by Jonathan Olds (MIT license).
 * See jaero_dsp/LICENSE for full terms.
 * Qt dependencies stripped for standalone C++ use. */
#ifndef JCONVOLUTIONALCODEC_H
#define JCONVOLUTIONALCODEC_H

//new viterbi
#ifdef __cplusplus
  extern "C" {
#endif
#include "correct.h"
#ifdef __cplusplus
  }
#endif

#include <vector>
#include <cstdint>

class JConvolutionalCodec
{
public:
    explicit JConvolutionalCodec();
    ~JConvolutionalCodec();
    void SetCode(int inv_rate, int order, const std::vector<uint16_t> poly, int paddinglength=24*4);
    std::vector<int> &Decode_Continuous(std::vector<uint8_t>& soft_bits_in);//0-->-1 128-->0 255-->1
    std::vector<int> &Decode_Continuous_hard(const std::vector<uint8_t>& soft_bits_in);//0-->1

    std::vector<int> &Decode_soft(std::vector<uint8_t>& soft_bits_in, int size);//0-->-1 128-->0 255-->1
    std::vector<int> &Decode_hard(const std::vector<uint8_t>& soft_bits_in, int size);//0-->1

    std::vector<int> &Soft_To_Hard_Convert(const std::vector<uint8_t>& soft_bits_in);//0-->-1 128-->0 255-->1

    std::vector<uint8_t> &Hard_To_Soft_Convert(std::vector<uint8_t>& hard_bits_in);//0-->-1 128-->0 255-->1

    int getPaddinglength(){return paddinglength;}
private:
    correct_convolutional *convol;
    int constraint;
    int nparitybits;
    int paddinglength;
    std::vector<uint8_t> soft_bits_overlap_buffer_uchar;
    std::vector<int> decoded_bits;//unpacked
    std::vector<uint8_t> decoded;//packed tempory storage
};

#endif // JCONVOLUTIONALCODEC_H
