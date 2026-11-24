// SPDX-License-Identifier: Apache-2.0
// Objective-C++ glue for an AU wrapper: converts the CFPropertyList a host passes to and from the JSON a Handler uses.
// Neither function throws or raises: input that is not a JSON-compatible dictionary gives "{}" (which a Handler answers with bad_request) or nullptr.
#pragma once
#include <CoreFoundation/CoreFoundation.h>
#import <Foundation/Foundation.h>
#include <string>

namespace presetrelay {

inline std::string cfToJson(CFPropertyListRef plist) {
    if (plist == nullptr) return "{}";
    @try {
        id obj = (__bridge id)plist;
        if (![obj isKindOfClass:[NSDictionary class]] || ![NSJSONSerialization isValidJSONObject:obj]) return "{}";
        NSData* d = [NSJSONSerialization dataWithJSONObject:obj options:0 error:nil];
        return d ? std::string((const char*)d.bytes, d.length) : std::string("{}");
    } @catch (...) {
        return "{}";
    }
}

// Returns a +1 retained CFPropertyList (a dictionary), or nullptr.
inline CFPropertyListRef jsonToCF(const std::string& json) {
    @try {
        NSData* d = [NSData dataWithBytes:json.data() length:json.size()];
        id obj = [NSJSONSerialization JSONObjectWithData:d options:0 error:nil];
        return [obj isKindOfClass:[NSDictionary class]] ? CFRetain((__bridge CFTypeRef)obj) : nullptr;
    } @catch (...) {
        return nullptr;
    }
}

}  // namespace presetrelay
