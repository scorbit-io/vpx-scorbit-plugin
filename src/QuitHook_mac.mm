#include "QuitHook.h"

#import <AppKit/AppKit.h>

namespace Scorbit
{

static id observer = nil;

void InstallQuitHook(void (*onQuit)())
{
   RemoveQuitHook();
   // queue:nil delivers synchronously on the posting thread. -[NSApplication terminate:]
   // posts this and then calls exit(), so a queued block would never run.
   observer = [[NSNotificationCenter defaultCenter]
      addObserverForName:NSApplicationWillTerminateNotification
                  object:nil
                   queue:nil
              usingBlock:^(NSNotification*) { onQuit(); }];
}

void RemoveQuitHook()
{
   if (observer == nil)
      return;
   [[NSNotificationCenter defaultCenter] removeObserver:observer];
   observer = nil;
}

}
