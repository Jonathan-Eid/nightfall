// Lifetime sentinel for Meteor's existing Windows session supervisor. The
// in-process OpenXR runtime owns rendering/tracking; no compositor daemon needed.
#include <windows.h>
#include <cstdio>
#include <cstring>
int main(int argc,char **argv){
    if(argc!=2 || std::strcmp(argv[1],"--service")){std::fputs("Use --service through a Meteor runtime profile\n",stderr);return 2;}
    std::puts("METEOR_OVR_SERVICE ready");std::fflush(stdout);
    for(;;)Sleep(1000);
}
