// Move to Trash via Foundation, exactly like Finder does (per-volume trash, name clashes).
#import <Foundation/Foundation.h>

#include <string>

bool macTrash(const std::string& path, std::string& err) {
  @autoreleasepool {
    NSString* s = [[NSFileManager defaultManager] stringWithFileSystemRepresentation:path.c_str()
                                                                           length:path.size()];
    if (!s) {
      err = "invalid file name";
      return false;
    }
    NSError* e = nil;
    BOOL ok = [[NSFileManager defaultManager] trashItemAtURL:[NSURL fileURLWithPath:s]
                                            resultingItemURL:nil
                                                       error:&e];
    if (!ok) err = e ? std::string([[e localizedDescription] UTF8String]) : "unknown error";
    return ok;
  }
}
