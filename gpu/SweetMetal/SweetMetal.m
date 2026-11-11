/*
 * SweetMetal.bundle - Metal driver plugin for iPadOS on the Raspberry Pi 5.
 *
 * Step 1: a logging skeleton. The kernel (SweetPlatformExpert.cpp, boot-arg
 * sweetmetal=1) publishes IOAcceleratorES with MetalPluginName "SweetMetal" and
 * MetalPluginClassName "SweetMTLDevice"; Metal.framework then loads this bundle
 * from /System/Library/Extensions and calls -initWithAcceleratorPort:. Every
 * MTLDevice call we do not implement is logged to the console (with the
 * protocol's own type signature) and answered with 0 / nil, so one boot shows
 * which parts of Metal QuartzCore, UIKit, the wallpaper and widgets need.
 *
 * Built by build.sh with the xtool Darwin SDK; installed by the image tools.
 */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern const char *getprogname(void);

static void
mlog(const char *fmt, ...)
{
	char line[768];
	int n = snprintf(line, sizeof(line), "SWEET METAL: %s[%d] ", getprogname(), getpid());
	va_list ap;
	va_start(ap, fmt);
	n += vsnprintf(line + n, sizeof(line) - (size_t)n - 2, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 2) {
		n = (int)sizeof(line) - 2;
	}
	line[n++] = '\n';
	/* Reopened per line: launchd daemons have no usable stderr. */
	int fd = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (fd >= 0) {
		(void)write(fd, line, (size_t)n);
		close(fd);
	}
}

static Protocol *
mtl_device_protocol(void)
{
	return objc_getProtocol("MTLDevice");
}

/* The protocol's own encoding for sel, required or optional, or NULL. */
static const char *
mtl_types(SEL sel)
{
	Protocol *p = mtl_device_protocol();
	if (p == NULL) {
		return NULL;
	}
	for (int req = 1; req >= 0; req--) {
		struct objc_method_description d = protocol_getMethodDescription(p, sel, (BOOL)req, YES);
		if (d.types != NULL) {
			return d.types;
		}
	}
	return NULL;
}

@interface SweetMTLDevice : NSObject
@end

@implementation SweetMTLDevice {
	uint32_t _port;
}

+ (void)load
{
	Protocol *p = mtl_device_protocol();
	if (p != NULL) {
		class_addProtocol(self, p);
	}
	mlog("SweetMetal.bundle loaded (MTLDevice protocol %s)", p ? "found" : "MISSING");
}

- (instancetype)initWithAcceleratorPort:(uint32_t)port
{
	if ((self = [super init])) {
		_port = port;
	}
	mlog("initWithAcceleratorPort: %u -> device %p", port, self);
	return self;
}

- (instancetype)initWithAcceleratorPort:(uint32_t)port options:(id)options
{
	mlog("initWithAcceleratorPort:options: %u %s", port, [[options description] UTF8String] ?: "(nil)");
	return [self initWithAcceleratorPort:port];
}

- (instancetype)initWithAcceleratorPort:(uint32_t)port simultaneousInstances:(unsigned)n
{
	mlog("initWithAcceleratorPort:simultaneousInstances: %u %u", port, n);
	return [self initWithAcceleratorPort:port];
}

/* Identity: enough for MTLCreateSystemDefaultDevice and logging by clients. */
- (NSString *)name
{
	return @"Raspberry Pi 5 (SweetMetal)";
}

- (uint64_t)registryID
{
	return 0x5157ee70;
}

- (BOOL)isLowPower { return NO; }
- (BOOL)isHeadless { return NO; }
- (BOOL)isRemovable { return NO; }
- (BOOL)hasUnifiedMemory { return YES; }

- (BOOL)supportsFamily:(NSInteger)family
{
	mlog("supportsFamily: %ld -> NO", (long)family);
	return NO;
}

- (BOOL)supportsFeatureSet:(NSUInteger)featureSet
{
	mlog("supportsFeatureSet: %lu -> NO", (unsigned long)featureSet);
	return NO;
}

/* Everything else: log it with its real signature and answer 0 / nil. */
- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel
{
	NSMethodSignature *m = [super methodSignatureForSelector:sel];
	if (m != nil) {
		return m;
	}
	const char *types = mtl_types(sel);
	return [NSMethodSignature signatureWithObjCTypes:types ?: "@@:"];
}

- (BOOL)respondsToSelector:(SEL)sel
{
	if ([super respondsToSelector:sel]) {
		return YES;
	}
	/* Report MTLDevice methods as present so clients call them (and we log). */
	BOOL r = mtl_types(sel) != NULL;
	mlog("respondsToSelector: %s -> %s", sel_getName(sel), r ? "YES (forwarded)" : "NO");
	return r;
}

- (void)forwardInvocation:(NSInvocation *)inv
{
	NSMethodSignature *sig = inv.methodSignature;
	mlog("-%s (returns %s) not implemented: answering 0/nil", sel_getName(inv.selector), sig.methodReturnType);
	NSUInteger len = sig.methodReturnLength;
	if (len > 0) {
		void *zero = calloc(1, len);
		[inv setReturnValue:zero];
		free(zero);
	}
}

@end
