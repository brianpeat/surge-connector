// SPDX-License-Identifier: Apache-2.0
// Objective-C++ glue used by an AU wrapper: converts the CFPropertyList the host passes to/from the JSON the
// Handler uses. Apache License 2.0.
#pragma once
#include <CoreFoundation/CoreFoundation.h>
#import <Foundation/Foundation.h>
#include <string>

namespace presetconnector {

inline std::string cfToJson(CFPropertyListRef plist) {
    if (plist == nullptr || ![NSJSONSerialization isValidJSONObject:(__bridge id)plist]) return "{}";
    NSData* d = [NSJSONSerialization dataWithJSONObject:(__bridge id)plist options:0 error:nil];
    return d ? std::string((const char*)d.bytes, d.length) : "{}";
}

// Returns a +1 retained CFPropertyList (dictionary), or nullptr.
inline CFPropertyListRef jsonToCF(const std::string& json) {
    NSData* d = [NSData dataWithBytes:json.data() length:json.size()];
    id obj = [NSJSONSerialization JSONObjectWithData:d options:0 error:nil];
    return obj ? CFRetain((__bridge CFTypeRef)obj) : nullptr;
}

}  // namespace presetconnector
