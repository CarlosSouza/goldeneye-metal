/*
 * SDL3 builds here with SDL_VIDEO=OFF (the runtime owns the window layer),
 * but SDL.c still references two helpers that only the UIKit video driver
 * provides on iOS. Mirror of SDL_uikitvideo.m.
 */
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE

#import <UIKit/UIKit.h>
#include <stdbool.h>

bool SDL_IsIPad(void)
{
    return ([UIDevice currentDevice].userInterfaceIdiom == UIUserInterfaceIdiomPad);
}

bool SDL_IsAppleTV(void)
{
    return ([UIDevice currentDevice].userInterfaceIdiom == UIUserInterfaceIdiomTV);
}

#endif
