/*
 * libtwlremote.dylib: inside tvOS's PineBoard. Some televisions send no keys over HDMI-CEC, and then a box with no
 * remote paired cannot be used at all: not even to open Settings and pair one. So when Bluetooth comes up with nothing
 * paired, a screen asks for the box's own remote (Xiaomi's "MI RC") to be put in its pairing mode, and the remote is
 * looked for, connected and paired from here, with nobody pressing anything on the box. A key from anywhere (the
 * television's remote after all, a keyboard) takes the screen away, for good.
 *
 * The remote pairs "Just Works" and gives up 3.4 s after asking (tvos/src/twlbt.c): the request is answered at once
 * here too, in whichever of PineBoard's pairing agents it arrives.
 *
 * The first start also asks for the language, the way Apple's setup does (and nothing else of that setup). The keys
 * go to the application in front, the home screen (HeadBoard), so the list is drawn there, by this same library, and
 * moved in with the presses taken from HeadBoard's events; the screen above waits on top of it until there is a
 * remote. HeadBoard cannot write outside its container: the choice goes to PineBoard, which puts it into the user's
 * preferences and starts again, in that language. Between the two, notify(3) states:
 *   twl.language.wanted  1: ask (set by PineBoard as it starts, before HeadBoard exists)
 *   twl.remote.asked     1: the remote's screen is up, keys are not for the list yet
 *   twl.language.choice  the row chosen + 1
 *
 * /var/mobile/twl/no-remote-prompt: never shown (written when a key took it away);
 * /var/mobile/twl/remote-prompt: shown at every start, paired or not (to look at it);
 * /var/mobile/twl/language-chosen: the language has been asked for (written with the choice);
 * /var/mobile/twl/language-prompt: asked at every start (to look at it).
 */
#import <CoreBluetooth/CoreBluetooth.h>
#import <UIKit/UIKit.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define FLAG_DIR        "/var/mobile/twl"
#define FLAG_NEVER      FLAG_DIR "/no-remote-prompt"
#define FLAG_ALWAYS     FLAG_DIR "/remote-prompt"
#define FLAG_LANGUAGE   FLAG_DIR "/language-chosen"
#define FLAG_LANGUAGE_ALWAYS FLAG_DIR "/language-prompt"

#define PAIRING_JUST_WORKS 0

@interface CBPairingAgent : NSObject
@property (weak) id delegate;
- (NSArray *)retrievePairedPeers;
- (void)pairPeer:(id)peer;
- (void)unpairPeer:(id)peer;
- (void)respondToPairingRequest:(id)peer type:(long)type accept:(BOOL)accept data:(id)data;
- (id)parentManager;
@end

@interface CBCentralManager (Twilight)
- (CBPairingAgent *)sharedPairingAgent;
@end

@interface NSObject (Twilight)
- (id)peerWithInfo:(id)info;
@end

@interface UIWindow (Twilight)
+ (NSArray *)allWindowsIncludingInternalWindows:(BOOL)internal onlyVisibleWindows:(BOOL)visible;
@end

static void
say(NSString *format, ...)
{
	va_list ap;
	NSString *line;
	int fd;

	va_start(ap, format);
	line = [[NSString alloc] initWithFormat:format arguments:ap];
	va_end(ap);
	fprintf(stderr, "twl: remote: %s\n", line.UTF8String);
	if ((fd = open(FLAG_DIR "/remote.log", O_WRONLY | O_APPEND)) >= 0) {    /* (only if someone made it) */
		dprintf(fd, "%s[%d]: %s\n", getprogname(), getpid(), line.UTF8String);
		close(fd);
	}
}

static unsigned
buttons_pressed(void)
{
	unsigned n = 0;
	size_t size = sizeof(n);

	return sysctlbyname("kern.hakios_buttons", &n, &size, NULL, 0) == 0 ? n : 0;
}

@interface TwlLanguagePrompt : NSObject
- (void)start;
@end

static TwlLanguagePrompt *language;

#define STATE_WANTED    "twl.language.wanted"
#define STATE_REMOTE    "twl.remote.asked"
#define STATE_CHOICE    "twl.language.choice"

static uint64_t
state_get(const char *name)
{
	uint64_t state = 0;
	int token;

	if (notify_register_check(name, &token) == NOTIFY_STATUS_OK) {
		notify_get_state(token, &state);
		notify_cancel(token);
	}
	return state;
}

static void
state_set(const char *name, uint64_t state)
{
	static int tokens[3];
	static const char *names[3];
	int i;

	/* (a state lives as long as some token for its name: these are kept) */
	for (i = 0; i < 3 && names[i] != NULL && strcmp(names[i], name) != 0; i++) {
	}
	if (i == 3 || (names[i] == NULL && notify_register_check(name, &tokens[i]) != NOTIFY_STATUS_OK)) {
		return;
	}
	names[i] = name;
	notify_set_state(tokens[i], state);
	notify_post(name);
}
static UIWindow *window_on_top(void);


#define REMOTE_WIDTH    210
#define REMOTE_HEIGHT   800

static CAShapeLayer *
remote_shape(UIView *view, UIBezierPath *path, bool held)
{
	CAShapeLayer *shape = [CAShapeLayer layer];

	shape.frame = view.bounds;
	shape.path = path.CGPath;
	shape.lineWidth = 3;
	shape.strokeColor = [UIColor colorWithWhite:1 alpha:held ? 1 : 0.45].CGColor;
	shape.fillColor = held ? [UIColor whiteColor].CGColor : [UIColor clearColor].CGColor;
	[view.layer addSublayer:shape];
	if (held) {
		CABasicAnimation *pulse = [CABasicAnimation animationWithKeyPath:@"opacity"];

		pulse.fromValue = @1;
		pulse.toValue = @0.35;
		pulse.duration = 0.9;
		pulse.autoreverses = YES;
		pulse.repeatCount = HUGE_VALF;
		pulse.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseInEaseOut];
		[shape addAnimation:pulse forKey:@"pulse"];
	}
	return shape;
}

static void
remote_glyph(UIView *view, CGFloat x, CGFloat y, CGFloat size, NSString *symbol, bool held)
{
	UIImageView *glyph = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:symbol
	    withConfiguration:[UIImageSymbolConfiguration configurationWithPointSize:size
	    weight:UIImageSymbolWeightSemibold]]];

	glyph.tintColor = held ? [UIColor colorWithWhite:0.1 alpha:1] : [UIColor colorWithWhite:1 alpha:0.45];
	glyph.contentMode = UIViewContentModeCenter;
	glyph.frame = CGRectMake(x - 30, y - 30, 60, 60);
	[view addSubview:glyph];
}

static void
remote_round(UIView *view, CGFloat x, CGFloat y, CGFloat radius, NSString *symbol, bool held)
{
	remote_shape(view, [UIBezierPath bezierPathWithArcCenter:CGPointMake(x, y) radius:radius startAngle:0
	    endAngle:2 * M_PI clockwise:YES], held);
	if (symbol != nil) {
		remote_glyph(view, x, y, radius * 0.8, symbol, held);
	}
}

/* the Mi remote in outline, the two buttons to hold filled in and pulsing */
static UIView *
remote_picture(void)
{
	UIView *view = [[UIView alloc] init];
	UILabel *tv = [[UILabel alloc] initWithFrame:CGRectMake(22, 372, 56, 40)];
	CGFloat middle = REMOTE_WIDTH / 2.0;

	view.translatesAutoresizingMaskIntoConstraints = NO;
	view.bounds = CGRectMake(0, 0, REMOTE_WIDTH, REMOTE_HEIGHT);
	[view.widthAnchor constraintEqualToConstant:REMOTE_WIDTH].active = YES;
	[view.heightAnchor constraintEqualToConstant:REMOTE_HEIGHT].active = YES;
	remote_shape(view, [UIBezierPath bezierPathWithRoundedRect:CGRectMake(1.5, 1.5, REMOTE_WIDTH - 3,
	    REMOTE_HEIGHT - 3) cornerRadius:46], false);
	remote_round(view, middle, 62, 20, @"power", false);
	remote_round(view, middle, 124, 20, @"mic.fill", false);
	remote_round(view, middle, 250, 80, nil, false);
	remote_round(view, middle, 250, 40, nil, false);
	remote_shape(view, [UIBezierPath bezierPathWithRoundedRect:CGRectMake(22, 372, 56, 40) cornerRadius:20], true);
	tv.text = @"TV+";
	tv.font = [UIFont systemFontOfSize:19 weight:UIFontWeightHeavy];
	tv.textColor = [UIColor colorWithWhite:0.1 alpha:1];
	tv.textAlignment = NSTextAlignmentCenter;
	[view addSubview:tv];
	remote_round(view, middle, 392, 22, @"arrow.left", false);
	remote_round(view, REMOTE_WIDTH - 50, 392, 22, @"house.fill", true);
	for (int i = 0; i < 4; i++) {
		remote_shape(view, [UIBezierPath bezierPathWithRoundedRect:CGRectMake(i % 2 ? middle + 6 : 22,
		    440 + (i / 2) * 50, middle - 28, 36) cornerRadius:18], false);
	}
	remote_shape(view, [UIBezierPath bezierPathWithRoundedRect:CGRectMake(middle - 24, 556, 48, 140)
	    cornerRadius:24], false);
	remote_glyph(view, middle, 586, 18, @"plus", false);
	remote_glyph(view, middle, 666, 18, @"minus", false);
	return view;
}

@interface TwlRemotePrompt : NSObject <CBCentralManagerDelegate>
@end

static TwlRemotePrompt *prompt;

@implementation TwlRemotePrompt {
	CBCentralManager *central;
	CBPeripheral *remote;
	UIWindow *window;
	UILabel *status;
	NSTimer *timer;
	unsigned buttons_before;
	bool looked, forced, bonded;
}

- (void)start
{
	forced = access(FLAG_ALWAYS, F_OK) == 0;
	if (!forced && access(FLAG_NEVER, F_OK) == 0) {
		say(@"not wanted (" FLAG_NEVER ")");
		return;
	}
	buttons_before = forced ? buttons_pressed() : 0;
	central = [[CBCentralManager alloc] initWithDelegate:self queue:dispatch_get_main_queue() options:nil];
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		if (!self->looked) {
			say(@"no Bluetooth after 20 s");
		}
	});
}

- (bool)shown
{
	return window != nil;
}

- (void)centralManagerDidUpdateState:(CBCentralManager *)manager
{
	NSArray *paired;

	say(@"Bluetooth state %ld", (long)manager.state);
	if (manager.state != CBManagerStatePoweredOn || looked) {
		return;
	}
	looked = true;
	[manager sharedPairingAgent].delegate = self;
	paired = [[manager sharedPairingAgent] retrievePairedPeers];
	say(@"%lu paired, %u keys so far", (unsigned long)paired.count, buttons_pressed());
	/* (a pairing left from before proves nothing on the first start: the remote may be paired elsewhere since) */
	if (!forced && (buttons_pressed() != 0 || (paired.count != 0 && access(FLAG_LANGUAGE, F_OK) == 0))) {
		central = nil;
		return;
	}
	[self show];
}

- (UILabel *)label:(NSString *)text size:(CGFloat)size weight:(UIFontWeight)weight alpha:(CGFloat)alpha
{
	UILabel *label = [[UILabel alloc] init];

	label.text = text;
	label.font = [UIFont systemFontOfSize:size weight:weight];
	label.textColor = [UIColor colorWithWhite:1 alpha:alpha];
	label.textAlignment = NSTextAlignmentCenter;
	label.numberOfLines = 0;
	label.translatesAutoresizingMaskIntoConstraints = NO;
	return label;
}

- (void)show
{
	UIView *root;
	UILabel *title, *how, *skip;
	UIActivityIndicatorView *spinner;
	UIStackView *stack, *both;

	if ((window = window_on_top()) == nil) {
		say(@"no window scene yet");
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
			[self show];
		});
		return;
	}
	root = window.rootViewController.view;

	title = [self label:@"Connect your remote" size:76 weight:UIFontWeightBold alpha:1];
	how = [self label:@"On the Mi remote, hold the Home button and the Xiaomi TV button together for 5 seconds.\n"
	    @"Keep the remote close to the box."
	    size:40 weight:UIFontWeightRegular alpha:0.85];
	spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleLarge];
	spinner.color = [UIColor whiteColor];
	[spinner startAnimating];
	status = [self label:@"Looking for the remote" size:34 weight:UIFontWeightMedium alpha:0.7];
	skip = [self label:@"If your TV's remote controls this box, press any button on it to continue."
	    size:30 weight:UIFontWeightRegular alpha:0.5];

	stack = [[UIStackView alloc] initWithArrangedSubviews:@[title, how, spinner, status, skip]];
	stack.axis = UILayoutConstraintAxisVertical;
	stack.alignment = UIStackViewAlignmentCenter;
	stack.spacing = 44;
	[stack setCustomSpacing:90 afterView:how];
	[stack setCustomSpacing:24 afterView:spinner];
	[stack setCustomSpacing:110 afterView:status];
	both = [[UIStackView alloc] initWithArrangedSubviews:@[stack, remote_picture()]];
	both.axis = UILayoutConstraintAxisHorizontal;
	both.alignment = UIStackViewAlignmentCenter;
	both.spacing = 140;
	both.translatesAutoresizingMaskIntoConstraints = NO;
	[root addSubview:both];
	[NSLayoutConstraint activateConstraints:@[
		[both.centerXAnchor constraintEqualToAnchor:root.centerXAnchor],
		[both.centerYAnchor constraintEqualToAnchor:root.centerYAnchor],
		[stack.widthAnchor constraintLessThanOrEqualToAnchor:root.widthAnchor multiplier:0.58],
	]];
	window.alpha = 0;
	window.hidden = NO;
	[UIView animateWithDuration:0.5 animations:^{
		self->window.alpha = 1;
	}];
	state_set(STATE_REMOTE, 1);
	say(@"shown (window level %.0f)", window.windowLevel);
	timer = [NSTimer scheduledTimerWithTimeInterval:1 target:self selector:@selector(tick) userInfo:nil repeats:YES];
	[self scan];
}

- (void)scan
{
	remote = nil;
	status.text = @"Looking for the remote";
	[central scanForPeripheralsWithServices:nil options:nil];
}

- (void)tick
{
	if (window == nil) {
		return;
	}
	/* (a pairing in the list is not a remote that works: its keys are, or a pairing made here) */
	if (buttons_pressed() != buttons_before) {
		[self finish:nil forGood:true];
	}
}

- (void)finish:(NSString *)message forGood:(bool)for_good
{
	UIWindow *w = window;

	say(@"done: %@", message ?: @"a key was pressed");
	[timer invalidate];
	timer = nil;
	[central stopScan];
	unlink(FLAG_ALWAYS);
	if (for_good) {
		mkdir(FLAG_DIR, 0755);
		close(open(FLAG_NEVER, O_WRONLY | O_CREAT, 0644));
	}
	if (message != nil) {
		status.text = message;
	}
	window = nil;
	[UIView animateWithDuration:0.5 delay:message != nil ? 1.5 : 0 options:0 animations:^{
		w.alpha = 0;
	} completion:^(BOOL finished) {
		w.hidden = YES;
	}];
	state_set(STATE_REMOTE, 0);
}

static bool
is_remote(CBPeripheral *peripheral, NSDictionary *advertisement)
{
	NSString *name = advertisement[CBAdvertisementDataLocalNameKey] ?: peripheral.name;

	if ([name isEqualToString:@"MI RC"] || [name isEqualToString:@"Xiaomi RC"]) {
		return true;
	}
	for (CBUUID *service in advertisement[CBAdvertisementDataServiceUUIDsKey]) {
		if ([service isEqual:[CBUUID UUIDWithString:@"1812"]]) {       /* Human Interface Device */
			return [advertisement[CBAdvertisementDataIsConnectable] boolValue];
		}
	}
	return false;
}

- (void)centralManager:(CBCentralManager *)manager didDiscoverPeripheral:(CBPeripheral *)peripheral
    advertisementData:(NSDictionary *)advertisement RSSI:(NSNumber *)rssi
{
	if (window == nil || remote != nil || !is_remote(peripheral, advertisement)) {
		return;
	}
	say(@"found %@ (%@, %@ dBm)", peripheral.name, peripheral.identifier.UUIDString, rssi);
	remote = peripheral;
	[manager stopScan];
	status.text = @"Remote found. Connecting";
	[manager connectPeripheral:peripheral options:nil];
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		if (self->window != nil && self->remote == peripheral) {
			say(@"no pairing in 20 s: looking again");
			[manager cancelPeripheralConnection:peripheral];
			[self scan];
		}
	});
}

- (void)centralManager:(CBCentralManager *)manager didConnectPeripheral:(CBPeripheral *)peripheral
{
	say(@"connected");
	/* the remote asks to pair by itself as a rule; if it has not, it is asked */
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC / 2), dispatch_get_main_queue(), ^{
		if (self->window != nil && self->remote == peripheral && !self->bonded) {
			say(@"asking it to pair");
			[[manager sharedPairingAgent] pairPeer:peripheral];
		}
	});
}

- (void)centralManager:(CBCentralManager *)manager didFailToConnectPeripheral:(CBPeripheral *)peripheral
    error:(NSError *)error
{
	say(@"no connection: %@", error);
	if (window != nil && remote == peripheral) {
		/* the remote has forgotten a pairing kept here (it keeps one, and has been paired elsewhere since) */
		for (CBPeer *peer in [[manager sharedPairingAgent] retrievePairedPeers]) {
			if ([peer.identifier isEqual:peripheral.identifier]) {
				say(@"forgetting the old pairing");
				[[manager sharedPairingAgent] unpairPeer:peer];
			}
		}
		[self scan];
	}
}

- (void)centralManager:(CBCentralManager *)manager didDisconnectPeripheral:(CBPeripheral *)peripheral
    error:(NSError *)error
{
	say(@"disconnected: %@", error);
	if (window != nil && remote == peripheral) {
		[self scan];
	}
}

- (void)pairingAgent:(CBPairingAgent *)agent peerDidCompletePairing:(id)peer
{
	say(@"paired");
	bonded = true;
	if (window != nil) {
		[self finish:@"Remote connected" forGood:false];
	}
}

- (void)pairingAgent:(CBPairingAgent *)agent peerDidFailToCompletePairing:(id)peer error:(NSError *)error
{
	say(@"pairing failed: %@", error);
	if (window != nil && remote != nil) {
		[central cancelPeripheralConnection:remote];
		[self scan];
	}
}

- (void)pairingAgent:(CBPairingAgent *)agent peerDidRequestPairing:(id)peer type:(long)type passkey:(NSNumber *)passkey
{
	say(@"a pairing request of type %ld", type);
	if (type == PAIRING_JUST_WORKS && window != nil) {
		[agent respondToPairingRequest:peer type:type accept:YES data:nil];
	}
}

- (void)pairingAgent:(CBPairingAgent *)agent peerDidUnpair:(id)peer
{
}
@end


static UIWindow *
window_on_top(void)
{
	UIWindowScene *scene = nil;
	CGFloat level = 0;
	UIWindow *window;

	for (UIWindow *w in [UIWindow allWindowsIncludingInternalWindows:YES onlyVisibleWindows:NO]) {
		if (w.windowScene != nil && scene == nil) {
			scene = w.windowScene;
		}
		if (w.windowLevel > level) {
			level = w.windowLevel;
		}
	}
	if (scene == nil) {
		return nil;
	}
	window = [[UIWindow alloc] initWithWindowScene:scene];
	window.frame = scene.screen.bounds;
	window.windowLevel = level + 10;
	window.userInteractionEnabled = NO;
	window.rootViewController = [[UIViewController alloc] init];
	window.rootViewController.view.backgroundColor = [UIColor colorWithRed:0.04 green:0.04 blue:0.06 alpha:1];
	return window;
}

/* the languages PineBoard has, in the order of Apple's setup: name, language, locale */
static const char *const languages[][3] = {
	{ "English", "en", "en_US" },
	{ "English (UK)", "en-GB", "en_GB" },
	{ "English (Australia)", "en-AU", "en_AU" },
	{ "Español", "es", "es_ES" },
	{ "Español (Latinoamérica)", "es-419", "es_MX" },
	{ "简体中文", "zh-Hans", "zh_CN" },
	{ "繁體中文", "zh-Hant", "zh_TW" },
	{ "繁體中文（香港）", "zh-HK", "zh_HK" },
	{ "日本語", "ja", "ja_JP" },
	{ "Français", "fr", "fr_FR" },
	{ "Français (Canada)", "fr-CA", "fr_CA" },
	{ "Deutsch", "de", "de_DE" },
	{ "Русский", "ru", "ru_RU" },
	{ "Português (Brasil)", "pt-BR", "pt_BR" },
	{ "Português (Portugal)", "pt-PT", "pt_PT" },
	{ "Italiano", "it", "it_IT" },
	{ "한국어", "ko", "ko_KR" },
	{ "Türkçe", "tr", "tr_TR" },
	{ "Nederlands", "nl", "nl_NL" },
	{ "العربية", "ar", "ar_SA" },
	{ "ภาษาไทย", "th", "th_TH" },
	{ "Svenska", "sv", "sv_SE" },
	{ "Dansk", "da", "da_DK" },
	{ "Tiếng Việt", "vi", "vi_VN" },
	{ "Norsk bokmål", "nb", "nb_NO" },
	{ "Polski", "pl", "pl_PL" },
	{ "Suomi", "fi", "fi_FI" },
	{ "Bahasa Indonesia", "id", "id_ID" },
	{ "עברית", "he", "he_IL" },
	{ "Ελληνικά", "el", "el_GR" },
	{ "Română", "ro", "ro_RO" },
	{ "Magyar", "hu", "hu_HU" },
	{ "Čeština", "cs", "cs_CZ" },
	{ "Català", "ca", "ca_ES" },
	{ "Slovenčina", "sk", "sk_SK" },
	{ "Українська", "uk", "uk_UA" },
	{ "Hrvatski", "hr", "hr_HR" },
	{ "Bahasa Melayu", "ms", "ms_MY" },
	{ "हिन्दी", "hi", "hi_IN" },
};
#define LANGUAGES       (sizeof(languages) / sizeof(languages[0]))
#define ROW_HEIGHT      74
#define ROW_WIDTH       620
#define ROWS_SHOWN      11

static void (*original_send_event)(id, SEL, UIEvent *);
static void send_event(id self, SEL selector, UIEvent *event);

@implementation TwlLanguagePrompt {
	UIWindow *window;
	UIView *rows, *marker;
	NSMutableArray<UILabel *> *names;
	NSLayoutConstraint *scroll;
	unsigned chosen, first;
	NSTimeInterval remote_until;
}

- (bool)shown
{
	return window != nil;
}

- (void)start
{
	UIView *root, *clip;
	UILabel *title;
	Method method;

	if (window != nil || state_get(STATE_WANTED) != 1) {
		return;
	}
	if ((window = window_on_top()) == nil) {
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), dispatch_get_main_queue(), ^{
			[self start];
		});
		return;
	}
	method = class_getInstanceMethod(object_getClass([UIApplication sharedApplication]), @selector(sendEvent:));
	original_send_event = (void (*)(id, SEL, UIEvent *))method_setImplementation(method, (IMP)send_event);
	root = window.rootViewController.view;

	title = [[UILabel alloc] init];
	title.text = @"Language";
	title.font = [UIFont systemFontOfSize:76 weight:UIFontWeightBold];
	title.textColor = [UIColor whiteColor];
	title.translatesAutoresizingMaskIntoConstraints = NO;
	[root addSubview:title];

	clip = [[UIView alloc] init];
	clip.clipsToBounds = YES;
	clip.translatesAutoresizingMaskIntoConstraints = NO;
	[root addSubview:clip];
	rows = [[UIView alloc] init];
	rows.translatesAutoresizingMaskIntoConstraints = NO;
	[clip addSubview:rows];
	marker = [[UIView alloc] initWithFrame:CGRectMake(0, 0, ROW_WIDTH, ROW_HEIGHT - 8)];
	marker.backgroundColor = [UIColor whiteColor];
	marker.layer.cornerRadius = 14;
	[rows addSubview:marker];
	names = [NSMutableArray array];
	for (unsigned i = 0; i < LANGUAGES; i++) {
		UILabel *name = [[UILabel alloc] initWithFrame:CGRectMake(0, i * ROW_HEIGHT, ROW_WIDTH, ROW_HEIGHT - 8)];

		name.text = @(languages[i][0]);
		name.font = [UIFont systemFontOfSize:38 weight:UIFontWeightMedium];
		name.textAlignment = NSTextAlignmentCenter;
		[rows addSubview:name];
		[names addObject:name];
	}
	scroll = [rows.topAnchor constraintEqualToAnchor:clip.topAnchor];
	[NSLayoutConstraint activateConstraints:@[
		[clip.centerXAnchor constraintEqualToAnchor:root.centerXAnchor],
		[clip.centerYAnchor constraintEqualToAnchor:root.centerYAnchor constant:70],
		[clip.widthAnchor constraintEqualToConstant:ROW_WIDTH],
		[clip.heightAnchor constraintEqualToConstant:ROWS_SHOWN * ROW_HEIGHT - 8],
		[rows.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor],
		[rows.widthAnchor constraintEqualToConstant:ROW_WIDTH],
		[rows.heightAnchor constraintEqualToConstant:LANGUAGES * ROW_HEIGHT],
		scroll,
		[title.centerXAnchor constraintEqualToAnchor:root.centerXAnchor],
		[title.bottomAnchor constraintEqualToAnchor:clip.topAnchor constant:-36],
	]];
	[self choose:0 animated:false];
	window.hidden = NO;
	say(@"language: asked");
}

- (void)choose:(unsigned)row animated:(bool)animated
{
	chosen = row;
	if (row < first) {
		first = row;
	} else if (row >= first + ROWS_SHOWN) {
		first = row - ROWS_SHOWN + 1;
	}
	[UIView animateWithDuration:animated ? 0.18 : 0 animations:^{
		self->marker.frame = CGRectMake(0, row * ROW_HEIGHT, ROW_WIDTH, ROW_HEIGHT - 8);
		self->scroll.constant = -(CGFloat)self->first * ROW_HEIGHT;
		for (unsigned i = 0; i < LANGUAGES; i++) {
			self->names[i].textColor = i == row ? [UIColor colorWithWhite:0.1 alpha:1] : [UIColor whiteColor];
		}
		[self->window layoutIfNeeded];
	}];
}

- (void)press:(UIPressType)type
{
	NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];

	/* the key that takes the remote's screen away, or the remote's first ones, are not a choice */
	if (state_get(STATE_REMOTE) == 1) {
		remote_until = now + 1;
	}
	if (now < remote_until) {
		return;
	}
	if (type == UIPressTypeUpArrow && chosen > 0) {
		[self choose:chosen - 1 animated:true];
	} else if (type == UIPressTypeDownArrow && chosen + 1 < LANGUAGES) {
		[self choose:chosen + 1 animated:true];
	} else if (type == UIPressTypeSelect || type == UIPressTypePlayPause) {
		[self take];
	}
}

- (void)take
{
	UIWindow *w = window;

	say(@"language: row %u", chosen);
	state_set(STATE_CHOICE, chosen + 1);
	window = nil;
	[UIView animateWithDuration:0.5 animations:^{
		w.rootViewController.view.alpha = 0;
	} completion:^(BOOL finished) {
		/* (PineBoard starts again if the language changed, and this with it; if not, on with the home screen) */
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
			w.hidden = YES;
		});
	}];
}
@end

/* in PineBoard: the row chosen in HeadBoard */
static void
language_chosen(unsigned row)
{
	NSString *code, *locale, *before = [NSLocale preferredLanguages].firstObject;

	if (row >= LANGUAGES) {
		return;
	}
	code = @(languages[row][1]);
	locale = @(languages[row][2]);
	say(@"language: %@ (%@), was %@", code, locale, before);
	CFPreferencesSetValue(CFSTR("AppleLanguages"), (__bridge CFArrayRef)@[code], kCFPreferencesAnyApplication,
	    kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
	CFPreferencesSetValue(CFSTR("AppleLocale"), (__bridge CFStringRef)locale, kCFPreferencesAnyApplication,
	    kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
	CFPreferencesSynchronize(kCFPreferencesAnyApplication, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
	mkdir(FLAG_DIR, 0755);
	close(open(FLAG_LANGUAGE, O_WRONLY | O_CREAT, 0644));
	unlink(FLAG_LANGUAGE_ALWAYS);
	state_set(STATE_WANTED, 0);
	if ([before isEqualToString:code] || ([code isEqualToString:@"en"] && [before hasPrefix:@"en-US"])) {
		return;
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), dispatch_get_main_queue(), ^{
		exit(0);        /* launchd starts PineBoard again, and the home screen with it, in the new language */
	});
}

static void
send_event(id self, SEL selector, UIEvent *event)
{
	if ([language shown] && event.type == UIEventTypePresses) {
		for (UIPress *press in ((UIPressesEvent *)event).allPresses) {
			if (press.phase == UIPressPhaseBegan) {
				[language press:press.type];
			}
		}
		return;
	}
	original_send_event(self, selector, event);
}

/* -[CBPairingAgent handlePairingRequested:]: its argument the message's dictionary (peer, type, passkey) */
static void (*original_pairing_requested)(id, SEL, NSDictionary *);

static void
pairing_requested(CBPairingAgent *self, SEL selector, NSDictionary *args)
{
	NSNumber *type = args[@"kCBMsgArgPairingType"];
	id peer;

	if (type != nil && type.integerValue == PAIRING_JUST_WORKS && [prompt shown] &&
	    (peer = [[self parentManager] peerWithInfo:args]) != nil) {
		say(@"a Just Works pairing request: accepted");
		[self respondToPairingRequest:peer type:PAIRING_JUST_WORKS accept:YES data:nil];
		return;
	}
	original_pairing_requested(self, selector, args);
}

__attribute__((constructor)) static void
twl_remote_init(void)
{
	char path[1024];
	uint32_t size = sizeof(path);
	size_t length;
	uintptr_t slide;
	Method method;

	static int token, youtube;

	if (_NSGetExecutablePath(path, &size) != 0) {
		return;
	}
	if (strstr(path, "HeadBoard") != NULL) {
		dispatch_async(dispatch_get_main_queue(), ^{
			language = [[TwlLanguagePrompt alloc] init];
			[language start];
		});
		return;
	}
	if (strstr(path, "PineBoard") == NULL) {
		return;
	}
	state_set(STATE_REMOTE, 0);
	state_set(STATE_CHOICE, 0);
	state_set(STATE_WANTED, access(FLAG_LANGUAGE_ALWAYS, F_OK) == 0 || access(FLAG_LANGUAGE, F_OK) != 0);
	notify_register_dispatch(STATE_CHOICE, &token, dispatch_get_main_queue(), ^(int t) {
		uint64_t row = 0;

		notify_get_state(t, &row);
		if (row != 0) {
			language_chosen((unsigned)row - 1);
		}
	});

	notify_register_dispatch("twl.key.youtube", &youtube, dispatch_get_main_queue(), ^(int t) {
		id service = ((id (*)(id, SEL))objc_msgSend)((id)objc_getClass("FBSSystemService"), sel_registerName("sharedService"));

		((void (*)(id, SEL, id, id, id))objc_msgSend)(service, sel_registerName("openApplication:options:withResult:"),
		    @"com.google.ios.youtube", nil, ^(NSError *error) {
			say(@"the YouTube button: %@", error == nil ? @"opened" : error);
		});
	});

	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - 0x180000000UL;
	method = class_getInstanceMethod(objc_getClass("CBPairingAgent"), sel_registerName("handlePairingRequested:"));
	if (method == NULL || (uintptr_t)method_getImplementation(method) != 0x18b5af384UL + slide) {
		say(@"-[CBPairingAgent handlePairingRequested:] is not the one known; left alone");
	} else {
		original_pairing_requested = (void (*)(id, SEL, NSDictionary *))method_setImplementation(method,
		    (IMP)pairing_requested);
	}
	/* (PineBoard's own start first: its windows, bluetoothd) */
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		prompt = [[TwlRemotePrompt alloc] init];
		[prompt start];
	});
}
