/*  mrprobe — load the driver bundle the way coreaudiod does, in a process we can debug.
 *
 *  Installing a HAL plugin costs a root copy and a coreaudiod restart, and when it does not
 *  appear there is nothing to read: the failure is silent and indistinguishable from a
 *  driver that loaded and published nothing. This does the same three steps coreaudiod does
 *  — CFPlugIn instance, QueryInterface, Initialize — and says which one failed.
 *
 *      clang -o mrprobe tools/mrprobe.c -framework CoreFoundation -framework CoreAudio
 *      ./mrprobe build/MixerReturn.driver
 *
 *  It cannot prove the HAL will accept the driver, only that the bundle, the plist wiring
 *  and the factory are sound. That is exactly the half that is otherwise invisible.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <CoreAudio/AudioServerPlugIn.h>
#include <stdio.h>

int main(int argc, const char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: mrprobe <path to .driver>\n"); return 1; }

    CFStringRef path = CFStringCreateWithCString(NULL, argv[1], kCFStringEncodingUTF8);
    CFURLRef    url  = CFURLCreateWithFileSystemPath(NULL, path, kCFURLPOSIXPathStyle, true);
    CFBundleRef bundle = CFBundleCreate(NULL, url);

    if (bundle == NULL) { printf("FAIL  CFBundleCreate — not a readable bundle\n"); return 2; }
    printf("ok    bundle opened\n");

    /*  What the plist claims, read back through CFBundle rather than plutil, because this
     *  is the view CFPlugIn itself uses. */
    CFDictionaryRef info = CFBundleGetInfoDictionary(bundle);
    CFDictionaryRef factories = info ? CFDictionaryGetValue(info, CFSTR("CFPlugInFactories")) : NULL;
    CFDictionaryRef types     = info ? CFDictionaryGetValue(info, CFSTR("CFPlugInTypes")) : NULL;
    printf("%s    CFPlugInFactories present\n", factories ? "ok  " : "FAIL");
    printf("%s    CFPlugInTypes present\n",     types     ? "ok  " : "FAIL");

    /*  The step that actually matters: ask CFPlugIn for an instance of the AudioServerPlugIn
     *  type. This is what fails when the factory symbol is missing, misnamed, or stripped. */
    CFArrayRef found = CFPlugInFindFactoriesForPlugInTypeInPlugIn(
        kAudioServerPlugInTypeUUID, CFBundleGetPlugIn(bundle));

    if (found == NULL || CFArrayGetCount(found) == 0) {
        printf("FAIL  no factory registered for the AudioServerPlugIn type\n");
        printf("      the bundle loads but coreaudiod would find nothing to instantiate\n");
        return 3;
    }
    printf("ok    %ld factory(ies) registered for the plug-in type\n",
           (long) CFArrayGetCount(found));

    CFUUIDRef factoryID = (CFUUIDRef) CFArrayGetValueAtIndex(found, 0);
    IUnknownVTbl** unknown =
        (IUnknownVTbl**) CFPlugInInstanceCreate(NULL, factoryID, kAudioServerPlugInTypeUUID);

    if (unknown == NULL) {
        printf("FAIL  CFPlugInInstanceCreate — the factory returned nothing\n");
        printf("      usually the factory function name in the plist does not match an\n"
               "      exported symbol, or the type UUID it checks for is wrong\n");
        return 4;
    }
    printf("ok    factory returned an instance\n");

    AudioServerPlugInDriverRef driver = NULL;
    const HRESULT hr = (*unknown)->QueryInterface(
        unknown, CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID), (LPVOID*) &driver);

    if (hr != S_OK || driver == NULL) {
        printf("FAIL  QueryInterface for the driver interface returned 0x%x\n", (unsigned) hr);
        return 5;
    }
    printf("ok    QueryInterface returned the driver interface\n");

    const OSStatus err = (*driver)->Initialize(driver, (AudioServerPlugInHostRef) NULL);
    printf("%s    Initialize returned %d\n", err == 0 ? "ok  " : "FAIL", (int) err);

    /*  And the question the device list cannot answer: does the plug-in object actually
     *  own a device? A plug-in that owns nothing publishes nothing. */
    AudioObjectPropertyAddress addr = {
        kAudioObjectPropertyOwnedObjects,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectID owned[8] = {0};
    UInt32 size = sizeof(owned);
    const OSStatus perr = (*driver)->GetPropertyData(
        driver, kAudioObjectPlugInObject, 0, &addr, 0, NULL, sizeof(owned), &size, owned);

    printf("%s    plug-in owns %u object(s)", perr == 0 ? "ok  " : "FAIL",
           (unsigned) (size / sizeof(AudioObjectID)));
    for (UInt32 i = 0; i < size / sizeof(AudioObjectID); ++i) printf("  id=%u", owned[i]);
    printf("\n");

    addr.mSelector = kAudioPlugInPropertyDeviceList;
    size = sizeof(owned);
    const OSStatus derr = (*driver)->GetPropertyData(
        driver, kAudioObjectPlugInObject, 0, &addr, 0, NULL, sizeof(owned), &size, owned);
    printf("%s    device list reports %u device(s)\n", derr == 0 ? "ok  " : "FAIL",
           (unsigned) (size / sizeof(AudioObjectID)));

    return 0;
}
