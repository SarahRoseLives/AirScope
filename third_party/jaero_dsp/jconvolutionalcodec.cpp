/* Originally from JAERO by Jonathan Olds (MIT license).
 * See jaero_dsp/LICENSE for full terms.
 * Qt dependencies stripped for standalone C++ use. */
#include "jconvolutionalcodec.h"

#include <cstdio>
#include <assert.h>
#include <math.h>
#include <iostream>
#include <cstring>

JConvolutionalCodec::JConvolutionalCodec()
{
    paddinglength=24*4;

    correct_convolutional_polynomial_t poly[2];
    poly[0]=109;
    poly[1]=79;
    convol = correct_convolutional_create(2, 7, poly);
    constraint=7;
    nparitybits=2;
}

void JConvolutionalCodec::SetCode(int inv_rate, int order,const std::vector<uint16_t>poly,int _paddinglength)
{
    correct_convolutional_destroy(convol);
    convol = correct_convolutional_create(inv_rate, order, poly.data());
    constraint=order;
    nparitybits=inv_rate;
    soft_bits_overlap_buffer_uchar.clear();
    paddinglength=_paddinglength;

}

JConvolutionalCodec::~JConvolutionalCodec()
{
    correct_convolutional_destroy(convol);
}

std::vector<int> &JConvolutionalCodec::Decode_hard(const std::vector<uint8_t>& soft_bits_in, int size)//0-->-1 128-->0 255-->1
{

    soft_bits_overlap_buffer_uchar.clear();

    //pack into bytes
    std::vector<uint8_t> hard_bytes_in;
    uint8_t uchar_cnt=0;
    uint8_t hard_byte=0;
    int bit_counter=0;
    for(int i=0;i<size||uchar_cnt;i++)
    {
        hard_byte<<=1;
        if(i<(int)soft_bits_in.size())
        {
            if(soft_bits_in[i]>0)hard_byte|=1;
            bit_counter++;
        }
        uchar_cnt++;
        if(uchar_cnt>=8)
        {
            uchar_cnt=0;
            hard_bytes_in.push_back(hard_byte);
            hard_byte=0;
        }
    }

    soft_bits_overlap_buffer_uchar.insert(soft_bits_overlap_buffer_uchar.end(), hard_bytes_in.begin(), hard_bytes_in.end());


    //decoce
    decoded.resize(hard_bytes_in.size()/nparitybits);
    size_t dbits=correct_convolutional_decode(convol,(uint8_t*)soft_bits_overlap_buffer_uchar.data(),bit_counter,(uint8_t*)decoded.data());
    assert(dbits>0);
    dbits=bit_counter/nparitybits;

    //unpack bytes
    decoded_bits.resize(dbits);
    int bit_ptr=0;
    for(int i=0;i<(int)decoded.size()&&bit_ptr<((int)dbits);i++)
    {
        uint8_t uch=decoded[i];
        for(int k=0;k<8;k++)
        {
            if(uch&128)decoded_bits[bit_ptr]=1;
             else decoded_bits[bit_ptr]=0;
            uch<<=1;
            bit_ptr++;
        }
    }

    return decoded_bits;

}
std::vector<int> &JConvolutionalCodec::Decode_soft(std::vector<uint8_t>& soft_bits_in, int size)//0-->-1 128-->0 255-->1
{

    soft_bits_overlap_buffer_uchar.clear();
    soft_bits_overlap_buffer_uchar.insert(soft_bits_overlap_buffer_uchar.end(), soft_bits_in.begin(), soft_bits_in.end());

    //decode
    decoded.resize(size/nparitybits);
    size_t dbits=correct_convolutional_decode_soft(convol,(uint8_t*)soft_bits_overlap_buffer_uchar.data(),size,(uint8_t*)decoded.data());
    assert(dbits>0);
    dbits=size/nparitybits;

    //unpack bytes
    decoded_bits.resize(dbits);
    int bit_ptr=0;
    for(int i=0;i<(int)decoded.size()&&bit_ptr<((int)dbits);i++)
    {
        uint8_t uch=decoded[i];
        for(int k=0;k<8;k++)
        {
            if(uch&128)decoded_bits[bit_ptr]=1;
             else decoded_bits[bit_ptr]=0;
            uch<<=1;
            bit_ptr++;
        }
    }

    return decoded_bits;

}


std::vector<int> &JConvolutionalCodec::Soft_To_Hard_Convert(const std::vector<uint8_t>& soft_bits_in)
{
    decoded_bits.clear();
    for(int i=0;i<(int)soft_bits_in.size();i++)
    {
        if(soft_bits_in[i]>=128)decoded_bits.push_back(1);
         else decoded_bits.push_back(0);
    }
    return decoded_bits;
}

std::vector<uint8_t>& JConvolutionalCodec::Hard_To_Soft_Convert(std::vector<uint8_t>& hard_bits_in)
{

    for(int i=0;i<(int)hard_bits_in.size();i++)
    {
        if(hard_bits_in[i]==0)
        {
            hard_bits_in[i] = (uint8_t)(0);
        }
         else
         {
            hard_bits_in[i] = (uint8_t)(255);

         }
    }
    return hard_bits_in;
}

std::vector<int> &JConvolutionalCodec::Decode_Continuous(std::vector<uint8_t>& soft_bits_in)//0-->-1 128-->0 255-->1
{
    int k=62;// polys.size()*(paddinglength+constraint_));

    soft_bits_overlap_buffer_uchar.insert(soft_bits_overlap_buffer_uchar.end(), soft_bits_in.begin(), soft_bits_in.end());

    // add some padding on the back
    std::vector<uint8_t> filldata(paddinglength, 128);
    soft_bits_overlap_buffer_uchar.insert(soft_bits_overlap_buffer_uchar.end(), filldata.begin(), filldata.end());


    //decode
    double bt_size = soft_bits_overlap_buffer_uchar.size();
    bt_size=(bt_size/nparitybits)/8+1;

    decoded.resize((soft_bits_overlap_buffer_uchar.size()/nparitybits)+1);

    size_t dbits=correct_convolutional_decode_soft(convol,(uint8_t*)soft_bits_overlap_buffer_uchar.data(),soft_bits_overlap_buffer_uchar.size(),(uint8_t*)decoded.data());
    assert(dbits>0);

    dbits=soft_bits_overlap_buffer_uchar.size()/nparitybits;

    //unpack bytes
    decoded_bits.resize(dbits);

    int bit_ptr=0;

    for(int i=0;i<(int)decoded.size()&&bit_ptr<((int)dbits);i++)
    {
        uint8_t uch=decoded[i];

        for(int k=0;k<8&&bit_ptr<((int)dbits);k++)
        {
            if(uch&128)decoded_bits[bit_ptr]=1;
             else decoded_bits[bit_ptr]=0;
            uch<<=1;
            bit_ptr++;
        }
    }


    //remove the padding bits from the return data
    {
        int start = paddinglength+1;
        int count = (int)soft_bits_in.size()/nparitybits;
        if(start < (int)decoded_bits.size() && count > 0) {
            if(start + count > (int)decoded_bits.size()) count = (int)decoded_bits.size() - start;
            std::vector<int> tmp(decoded_bits.begin() + start, decoded_bits.begin() + start + count);
            decoded_bits = tmp;
        } else {
            decoded_bits.clear();
        }
    }

    //remove the used data from the padding buffer
    {
        int sz = (int)soft_bits_in.size();
        if(sz >= k) {
            soft_bits_overlap_buffer_uchar.assign(soft_bits_in.end() - k, soft_bits_in.end());
        } else {
            soft_bits_overlap_buffer_uchar = soft_bits_in;
        }
        soft_bits_overlap_buffer_uchar.resize(k);
    }

    return decoded_bits;
}

    // unpack the re-encoded bytes
std::vector<int> &JConvolutionalCodec::Decode_Continuous_hard(const std::vector<uint8_t>& soft_bits_in)//0-->-1 128-->0 255-->1
{
    int k=(2*nparitybits*paddinglength)/8;//msg is padded frount and back both times by paddinglength
    if((int)soft_bits_overlap_buffer_uchar.size()!=k) {
        soft_bits_overlap_buffer_uchar.assign(k, 0);
    }

    //harden the bits up and pack into bytes
    std::vector<uint8_t> hard_bytes_in;
    uint8_t uchar_cnt=0;
    uint8_t hard_byte=0;
    int bit_counter=0;
    for(int i=0;i<(int)soft_bits_in.size()||uchar_cnt;i++)
    {
        hard_byte<<=1;
        if(i<(int)soft_bits_in.size())
        {
            if(soft_bits_in[i]>0)hard_byte|=1;
            bit_counter++;
        }
        uchar_cnt++;
        if(uchar_cnt>=8)
        {
            uchar_cnt=0;
            hard_bytes_in.push_back(hard_byte);
            hard_byte=0;
        }
    }

    soft_bits_overlap_buffer_uchar.insert(soft_bits_overlap_buffer_uchar.end(), hard_bytes_in.begin(), hard_bytes_in.end());

    //decode hard
    decoded.resize((soft_bits_overlap_buffer_uchar.size()/nparitybits)+1);
    size_t dbits=correct_convolutional_decode(convol,(uint8_t*)soft_bits_overlap_buffer_uchar.data(),bit_counter+k*8,(uint8_t*)decoded.data());
    assert(dbits>0);
    dbits=(bit_counter+k*8)/nparitybits;

    //unpack bytes
    decoded_bits.resize(dbits);
    int bit_ptr=0;
    for(int i=0;i<(int)decoded.size()&&bit_ptr<((int)dbits);i++)
    {
        uint8_t uch=decoded[i];
        for(int k=0;k<8;k++)
        {
            if(uch&128)decoded_bits[bit_ptr]=1;
             else decoded_bits[bit_ptr]=0;
            uch<<=1;
            bit_ptr++;
        }
    }

    //remove the padding bits from the return data
    {
        int start = paddinglength;
        if(start < (int)decoded_bits.size()) {
            decoded_bits.erase(decoded_bits.begin(), decoded_bits.begin() + start);
        }
    }
    {
        int trim = paddinglength;
        if(trim < (int)decoded_bits.size()) {
            decoded_bits.resize(decoded_bits.size() - trim);
        }
    }

    //remove the used data from the padding buffer
    {
        int sz = (int)soft_bits_overlap_buffer_uchar.size();
        if(sz > k) {
            soft_bits_overlap_buffer_uchar.erase(soft_bits_overlap_buffer_uchar.begin(), soft_bits_overlap_buffer_uchar.end() - k);
        }
    }


    return decoded_bits;
}
