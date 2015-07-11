// Copyright 2015 runtime.js project authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

'use strict';

var isaac = require('../../deps/isaac/isaac');

// isaac.js returns crazy numbers, both positive and negative.
// This function is called on an isaac.rand() call,
// It gets the number to qualify for all of the following:
//  * Positive,
//  * Whole number,
//  * 0 < n < 256
// This ensures the numbers are similar to the numbers
// generated by VirtioRNG.
function isaacRound(n) {
  // Ensure it's positive
  if (n < 0) n = -n;
  // 0 < n < 256
  while (n > 256) n /= 4;
  // Whole number
  n = Math.round(n);

  return n;
}

var def = 'none';
var sources = {};

module.exports = {
  addSource: function(name, obj) {
    sources[name] = obj;
    // source.init() can do it's initialization stuff, BUT, it must return a seed for isaac.js
    var seed = sources[name].init();
    isaac.reset();
    isaac.seed(seed);
  },
  setDefault: function(name) {
    if (!sources[name]) {
      return false;
    }

    def = name;
    return true;
  },
  getRandomValues: function(length, cb, method) {
    // Works like /dev/random, always waits for real randomness.

    if (typeof length === 'undefined') {
      length = 1;
    }

    if (typeof length === 'function') {
      cb = length;
      length = 1;
    }

    if (typeof cb === 'undefined') {
      throw new Error('runtime.random.getRandomValues requires a callback');
    }

    // Don't use sources[method || def] (method may be defined,
    // but it may not be a method in sources).
    var method = sources[method || ''] || sources[def];

    method.queue.push({
      missing: length,
      array: new Uint8Array(length)
    });
    method.fillQueue(function() {
      var arr = method.queue.pop();
      cb(arr.array);
    });
  },
  getPseudoRandomValues: function(length, vari) {
    // This function asks for a seed and uses isaac.js (a CSPRNG)
    // to generate randomness from the seed to fill up the the request.

    if (typeof length === 'undefined') {
      length = 1;
    }

    if (length instanceof Uint8Array) {
      vari = length;
      length = 1;
    }

    if (vari) {
      if (!vari instanceof Uint8Array) {
        throw new Error('getPseudoRandomValues: variable must be an instance of Uint8Array');
      } else {
        for (var i = 0; i < vari.length; i++) {
          vari[i] = isaacRound(isaac.rand());
        }
        return vari;
      }
    } else {
      var arr = [];

      while (arr.length < length) {
        arr.push(isaacRound(isaac.rand()));
      }

      return new Uint8Array(arr);
    }
  }
};
