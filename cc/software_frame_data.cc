// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/software_frame_data.h"

namespace cc {
 
SoftwareFrameData::SoftwareFrameData()
    : content_dib(TransportDIB::DefaultHandleValue()) {
}
   
SoftwareFrameData::~SoftwareFrameData() {}
    
}  // namespace cc
