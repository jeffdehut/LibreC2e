// Exercise the real AppKit promise delegate, including the order Finder uses: the
// drag ends before the destination asks us to write. No WindowServer is required.
#import "../DragOutMac.m"
#include <stdlib.h>
#include <string.h>
// These callbacks ignore the session/provider arguments; nil avoids requiring a GUI.
#pragma clang diagnostic ignored "-Wnonnull"

#define CHECK(test) do { if (!(test)) { fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #test); exit(1); } } while (0)
typedef struct { int calls, written, error; } Probe;
static void completed(int written, const char* error, void* context) {
	Probe* probe = context;
	probe->calls++;
	probe->written = written;
	probe->error = error != NULL;
}
static OC1DragSource* make_source(NSString* path, Probe* probe) {
	OC1DragSource* source = [[OC1DragSource alloc] init];
	source->source_path = [path copy];
	source->done = completed;
	source->user_context = probe;
	return source;
}
static int native_menu_depth = 0;
static void native_menu_changed(int began) { native_menu_depth += began ? 1 : -1; }
int main(int argc, const char** argv) {
	@autoreleasepool {
		CHECK(argc == 2);
		if (!strcmp(argv[1], "menu_tracking")) {
			oc1_mac_watch_menu_tracking(native_menu_changed);
			NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
			[center postNotificationName:NSMenuDidBeginTrackingNotification object:nil];
			[center postNotificationName:NSMenuDidBeginTrackingNotification object:nil];
			CHECK(native_menu_depth == 2);
			[center postNotificationName:NSMenuDidEndTrackingNotification object:nil];
			[center postNotificationName:NSMenuDidEndTrackingNotification object:nil];
			CHECK(native_menu_depth == 0);
			return 0;
		}
		NSString* folder = [NSTemporaryDirectory() stringByAppendingPathComponent:[[NSUUID UUID] UUIDString]];
		NSFileManager* files = [NSFileManager defaultManager];
		CHECK([files createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:NULL]);
		NSString* path = [folder stringByAppendingPathComponent:@"Norn.exp"];
		NSData* original = [@"closed export payload" dataUsingEncoding:NSUTF8StringEncoding];
		CHECK([original writeToFile:path atomically:YES]);
		Probe probe = {0};
		OC1DragSource* source = make_source(path, &probe);
		NSString* target = [folder stringByAppendingPathComponent:@"received.exp"];
		BOOL expect_error = NO;
		if (!strcmp(argv[1], "drag_end_is_not_success")) {
			[source retain];
			[source draggingSession:nil endedAtPoint:NSZeroPoint operation:NSDragOperationCopy];
			CHECK(probe.calls == 0);
			CHECK([files fileExistsAtPath:path]);
		} else if (!strcmp(argv[1], "cancel")) {
			[source retain];
			[source draggingSession:nil endedAtPoint:NSZeroPoint operation:NSDragOperationNone];
			CHECK(probe.calls == 1 && !probe.written && !probe.error);
			expect_error = YES;
		} else if (!strcmp(argv[1], "missing_directory")) {
			target = [folder stringByAppendingPathComponent:@"missing/Norn.exp"];
			expect_error = YES;
		} else if (!strcmp(argv[1], "existing_destination")) {
			CHECK([@"keep existing file" writeToFile:target atomically:YES encoding:NSUTF8StringEncoding error:NULL]);
			expect_error = YES;
		} else if (!strcmp(argv[1], "missing_source")) {
			CHECK([files removeItemAtPath:path error:NULL]);
			expect_error = YES;
		} else if (!strcmp(argv[1], "abandoned_promise")) {
			[source release];
			CHECK(probe.calls == 1 && !probe.written);
			CHECK([files fileExistsAtPath:path]);
			CHECK([files removeItemAtPath:folder error:NULL]);
			return 0;
		} else {
			CHECK(!strcmp(argv[1], "successful_write"));
		}
		__block BOOL called = NO;
		[source filePromiseProvider:nil writePromiseToURL:[NSURL fileURLWithPath:target] completionHandler:^(NSError* error) {
			called = YES;
			CHECK((error != nil) == expect_error);
			if (!error) CHECK([[NSData dataWithContentsOfFile:target] isEqualToData:original]);
		}];
		CHECK(called && probe.calls == 1);
		CHECK(probe.written == !expect_error);
		if (strcmp(argv[1], "cancel")) CHECK(probe.error == expect_error);
		if (!strcmp(argv[1], "existing_destination")) CHECK([[NSString stringWithContentsOfFile:target encoding:NSUTF8StringEncoding error:NULL] isEqualToString:@"keep existing file"]);
		if (strcmp(argv[1], "missing_source")) CHECK([[NSData dataWithContentsOfFile:path] isEqualToData:original]);
		[source release];
		CHECK(probe.calls == 1);
		CHECK([files removeItemAtPath:folder error:NULL]);
	}
	return 0;
}
