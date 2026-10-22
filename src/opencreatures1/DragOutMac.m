// Finder's drag-ended callback accepts an operation, but does not confirm a file copy.
// Offer a file promise and report success only after writing and closing its destination.
#import <Cocoa/Cocoa.h>
#include <stdio.h>

typedef void (*oc1_drag_done)(int written, const char* error, void* context);

@interface OC1DragSource : NSObject <NSDraggingSource, NSFilePromiseProviderDelegate> {
  @public
	oc1_drag_done done;
	void* user_context;
	NSString* source_path;
}
- (void)finishWritten:(BOOL)written error:(NSError*)error;
@end

@implementation OC1DragSource
- (void)finishWritten:(BOOL)written error:(NSError*)error {
	if (!done) return;
	oc1_drag_done callback = done;
	done = NULL;
	callback(written, error ? [[error localizedDescription] UTF8String] : NULL, user_context);
}

- (void)dealloc {
	// A discarded promise must never imply a successful export.
	[self finishWritten:NO error:nil];
	[source_path release];
	[super dealloc];
}

- (NSDragOperation)draggingSession:(NSDraggingSession*)session sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
	(void)session;
	return context == NSDraggingContextOutsideApplication ? NSDragOperationCopy : NSDragOperationNone;
}

- (void)draggingSession:(NSDraggingSession*)session endedAtPoint:(NSPoint)point operation:(NSDragOperation)operation {
	(void)session;
	(void)point;
	if (operation == NSDragOperationNone) {
		[self finishWritten:NO error:nil];
	} else if (done) {
		// The destination can request the promised file AFTER this method returns.
		// Keep its delegate alive; an abandoned request eventually keeps the Norn.
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
			NSError* error = [NSError errorWithDomain:@"org.openc2e.export" code:1
				userInfo:@{NSLocalizedDescriptionKey: @"The destination did not finish saving the export. The Norn was kept."}];
			[self finishWritten:NO error:error];
		});
	}
	[self release]; // the dragging session's ownership; a pending promise retains its delegate
}

- (NSString*)filePromiseProvider:(NSFilePromiseProvider*)provider fileNameForType:(NSString*)type {
	(void)provider;
	(void)type;
	return [source_path lastPathComponent];
}

- (void)filePromiseProvider:(NSFilePromiseProvider*)provider writePromiseToURL:(NSURL*)url completionHandler:(void (^)(NSError*))completionHandler {
	(void)provider;
	[self retain]; // completion can release the receiver's pasteboard/provider
	NSError* error = nil;
	BOOL written = NO;
	if (!done) {
		error = [NSError errorWithDomain:NSCocoaErrorDomain code:NSUserCancelledError userInfo:nil];
	} else {
		// copyItem refuses an existing destination. Use the exact URL supplied by Finder.
		written = [[NSFileManager defaultManager] copyItemAtPath:source_path toPath:[url path] error:&error];
		if (written) {
			NSFileHandle* file = [NSFileHandle fileHandleForWritingToURL:url error:&error];
			written = file != nil;
			if (file) {
				NSError* close_error = nil;
				BOOL synced = [file synchronizeAndReturnError:&error];
				BOOL closed = [file closeAndReturnError:&close_error];
				written = synced && closed;
				if (!error) error = close_error;
			}
		}
	}
	if (written) fprintf(stdout, "INFO [Export] Finder destination written and closed: %s\n", [[url path] UTF8String]);
	[self finishWritten:written error:error];
	completionHandler(error);
	[self release];
}
@end

// Returns 0 if the drag couldn't start (done isn't called then).
int oc1_mac_drag_start(void* ns_window, const char* path, oc1_drag_done done, void* context) {
	NSWindow* window = (NSWindow*)ns_window;
	NSView* view = [window contentView];
	if (!view) return 0;
	NSString* file = [NSString stringWithUTF8String:path];
	OC1DragSource* source = [[OC1DragSource alloc] init];
	source->done = done;
	source->user_context = context;
	source->source_path = [file copy];
	NSFilePromiseProvider* promise = [[[NSFilePromiseProvider alloc] initWithFileType:@"public.data" delegate:source] autorelease];
	promise.userInfo = source; // the delegate property itself is weak
	NSDraggingItem* item = [[[NSDraggingItem alloc] initWithPasteboardWriter:promise] autorelease];
	NSImage* icon = [[NSWorkspace sharedWorkspace] iconForFile:file];
	NSPoint in_window = [window mouseLocationOutsideOfEventStream];
	NSPoint at = [view convertPoint:in_window fromView:nil];
	[item setDraggingFrame:NSMakeRect(at.x - 24, at.y - 24, 48, 48) contents:icon];
	NSEvent* event = [NSApp currentEvent];
	if (!event || ([event type] != NSEventTypeRightMouseDragged && [event type] != NSEventTypeLeftMouseDragged && [event type] != NSEventTypeOtherMouseDragged)) {
		event = [NSEvent mouseEventWithType:NSEventTypeRightMouseDragged
			location:in_window modifierFlags:[NSEvent modifierFlags]
			timestamp:[[NSProcessInfo processInfo] systemUptime] windowNumber:[window windowNumber]
			context:nil eventNumber:0 clickCount:1 pressure:1];
	}
	@try {
		NSDraggingSession* session = [view beginDraggingSessionWithItems:@[item] event:event source:source];
		if (!session) {
			source->done = NULL;
			[source release];
			return 0;
		}
	} @catch (NSException* e) {
		fprintf(stderr, "WARN [Export] Could not start Finder drag: %s\n", [[e reason] UTF8String]);
		source->done = NULL; // caller reports failure and still owns its callback context
		[source release];
		return 0;
	}
	return 1;
}

// Native menu tracking runs its own event loop while the player chooses an item.
// It must not be mistaken for an unresponsive simulation by the C1 watchdog.
void oc1_mac_watch_menu_tracking(void (*changed)(int)) {
	NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
	[center addObserverForName:NSMenuDidBeginTrackingNotification object:nil queue:nil
		usingBlock:^(NSNotification* notification) { (void)notification; changed(1); }];
	[center addObserverForName:NSMenuDidEndTrackingNotification object:nil queue:nil
		usingBlock:^(NSNotification* notification) { (void)notification; changed(0); }];
}
