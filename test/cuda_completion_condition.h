#pragma once
// Query only events successfully recorded after ALL accesses (including D2H).
// Failure retirement uses the stream, never a fresh/unrecorded event.
template<class Query,class Drain>
bool cuda_completion_ready(bool recorded,int& error,int success,int not_ready,Query query,Drain drain) {
    int status=success;
    if(recorded){status=query();if(status==not_ready)return false;}
    if(!recorded || status!=success){int stopped=drain();if(error==success)error=status!=success?status:stopped;}
    return true;
}
