//#pragma once
#ifndef TB_SYZYGY_H_
#define TB_SYZYGY_H_

#include "Type.h"

class Position;

namespace Tablebases {

    extern int32_t TBLargest;
    
    extern int32_t probe_wdl   (Position &pos, int32_t *success);
    extern int32_t probe_dtz   (Position &pos, int32_t *success);
    
    extern bool root_probe     (Position &pos, Value &TBScore);
    extern bool root_probe_wdl (Position &pos, Value &TBScore);

    extern void initialize     (const std::string &path);

}

#endif