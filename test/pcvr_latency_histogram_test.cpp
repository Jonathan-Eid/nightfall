#include "pcvr_latency_histogram.h"
#include <cassert>
#include <thread>
int main() {
    PCVRLatencyHistogram histogram;
    assert(histogram.percentile_upper_us(95)==0);
    for(unsigned i=0;i<100;++i) histogram.observe(i*1000);
    assert(histogram.count==100 && histogram.sum_us==4950000 && histogram.maximum_us==99000);
    assert(histogram.percentile_upper_us(50)==50000);
    assert(histogram.percentile_upper_us(95)==95000);
    histogram.reset();
    std::thread worker([&](){for(unsigned i=0;i<1000;++i) histogram.observe(200000);});
    for(unsigned i=0;i<1000;++i) histogram.observe(100);
    worker.join();
    assert(histogram.count==2000 && histogram.sum_us==200100000 && histogram.maximum_us==200000);
    assert(histogram.percentile_upper_us(50)==1000 && histogram.percentile_upper_us(95)==200000);
    histogram.reset();
    assert(histogram.count==0 && histogram.maximum_us==0 && histogram.percentile_upper_us(95)==0);
}
