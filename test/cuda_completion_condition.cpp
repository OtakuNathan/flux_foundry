#include "cuda_completion_condition.h"
#include <cstdlib>
#define CHECK(x) do{if(!(x))std::abort();}while(false)
int main(){
    int queried=0,drained=0,error=7;
    CHECK(cuda_completion_ready(false,error,0,1,[&]{++queried;return 0;},[&]{++drained;return 0;}));
    CHECK(queried==0&&drained==1&&error==7);
    error=0;CHECK(!cuda_completion_ready(true,error,0,1,[&]{++queried;return 1;},[&]{++drained;return 0;}));
    CHECK(drained==1);
    CHECK(cuda_completion_ready(true,error,0,1,[&]{++queried;return 2;},[&]{++drained;return 2;}));
    CHECK(error==2&&drained==2);
}
