#pragma once

#include "hdr_parallel.h"
#include <algorithm>
#include <vector>

template<class Sample>
void orient_image(std::vector<Sample>& pixels, unsigned& width, unsigned& height,
                  unsigned orientation, unsigned samples_per_pixel = 4) {
    if(pixels.empty() || orientation == 1)return;
    const unsigned w=width,h=height;
    if(orientation>=5)std::swap(width,height);
    std::vector<Sample> oriented(pixels.size());
    hdr_parallel_pixels(h,hdr_pixel_jobs(size_t(w)*h),[&](size_t begin,size_t end,unsigned) {
        for(unsigned y=begin;y<end;++y)for(unsigned x=0;x<w;++x) {
            unsigned dx=x,dy=y;
            switch(orientation) {
            case 2:dx=w-1-x;break;
            case 3:dx=w-1-x;dy=h-1-y;break;
            case 4:dy=h-1-y;break;
            case 5:dx=y;dy=x;break;
            case 6:dx=h-1-y;dy=x;break;
            case 7:dx=h-1-y;dy=w-1-x;break;
            case 8:dx=y;dy=w-1-x;break;
            }
            std::copy_n(pixels.data()+(size_t(y)*w+x)*samples_per_pixel,samples_per_pixel,
                        oriented.data()+(size_t(dy)*width+dx)*samples_per_pixel);
        }
    });
    pixels.swap(oriented);
}
