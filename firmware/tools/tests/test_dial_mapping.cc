#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
constexpr int kDialBandLow=880, kDialBandHigh=1080;
constexpr int kDialTrackLeft=16, kDialTrackRight=198;
std::size_t s_station_count=1, s_station_index=0;
uint16_t s_station_frequency=0;
int dial_x_for_station(){
    const int span=kDialTrackRight-kDialTrackLeft;
    if(s_station_frequency>=kDialBandLow && s_station_frequency<=kDialBandHigh){
        const int offset=(s_station_frequency-kDialBandLow)*span/(kDialBandHigh-kDialBandLow);
        return kDialTrackLeft+offset;
    }
    const int count=static_cast<int>(std::max<std::size_t>(s_station_count,1));
    const int index=static_cast<int>(std::min<std::size_t>(s_station_index,s_station_count-1));
    const int inset=span/8; const int usable=span-inset*2;
    const int offset=count>1?inset+usable*index/(count-1):inset+usable/2;
    return kDialTrackLeft+offset;
}
int main(){
    int fail=0;
    // real frequency mapping must match the printed scale
    struct{int f;int x;} anchors[]={{880,16},{920,52},{960,88},{1000,125},{1040,161},{1080,198}};
    for(auto&a:anchors){ s_station_frequency=a.f; int got=dial_x_for_station();
        if(std::abs(got-a.x)>1){printf("FAIL freq %d -> %d expect %d\n",a.f,got,a.x);++fail;} }
    // monotonic across the band
    int prev=-1; for(int f=880;f<=1080;++f){ s_station_frequency=f; int x=dial_x_for_station();
        if(x<prev){printf("FAIL non-monotonic at %d\n",f);++fail;break;} prev=x; }
    // needle always inside the track
    for(int f=880;f<=1080;++f){ s_station_frequency=f; int x=dial_x_for_station();
        if(x<kDialTrackLeft||x>kDialTrackRight){printf("FAIL out of track %d\n",f);++fail;break;} }
    // positional fallback: never pinned to the ends, always distinct, ordered
    s_station_frequency=0;
    for(int n=1;n<=10;++n){ s_station_count=n; int last=-1;
        for(int i=0;i<n;++i){ s_station_index=i; int x=dial_x_for_station();
            if(x<=kDialTrackLeft+4||x>=kDialTrackRight-4){printf("FAIL pinned n=%d i=%d x=%d\n",n,i,x);++fail;}
            if(n>1&&i>0&&x<=last){printf("FAIL not increasing n=%d i=%d\n",n,i);++fail;}
            last=x; } }
    // typical 6-station spread should be visibly separated
    s_station_count=6; printf("6-station fallback:");
    for(int i=0;i<6;++i){s_station_index=i;printf(" %d",dial_x_for_station());} printf("\n");
    printf(fail?"%d failed\n":"all dial checks passed\n",fail);
    return fail?1:0;
}
