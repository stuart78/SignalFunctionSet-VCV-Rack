/*
 * Copyright 2013 Google Inc.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *      http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __CONTROLLERS_H
#define __CONTROLLERS_H

// State of MIDI controllers

const int kControllerPitch = 128;

class Controllers {
 public:
  int values_[129];

  // --- SFS additions: live macros read each block by Dx7Note::compute ---
  bool opEnabled[6];      // per-operator on/off
  int  feedbackOffset;    // added to the patch feedback (clamped 0..7)
  float brightness;       // -1..+1; scales modulator-operator levels

  Controllers() {
    for (int i = 0; i < 129; i++) values_[i] = 0;
    values_[kControllerPitch] = 0x2000;
    for (int i = 0; i < 6; i++) opEnabled[i] = true;
    feedbackOffset = 0;
    brightness = 0.f;
  }
};

#endif  // __CONTROLLERS_H

