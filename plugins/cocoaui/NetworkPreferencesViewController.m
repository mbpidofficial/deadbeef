//
//  NetworkPreferencesViewController.m
//  DeaDBeeF
//
//  Created by Alexey Yakovenko on 2/23/20.
//  Copyright © 2020 Alexey Yakovenko. All rights reserved.
//

#import "NetworkPreferencesViewController.h"
#include "ctmap.h"
#include "deadbeef.h"

extern DB_functions_t *deadbeef;

static NSString *kContentTypeMappingChangedNotification = @"ContentTypeMappingChanged";

@interface ContentTypeMap : NSObject

- (instancetype)init NS_UNAVAILABLE;
- (instancetype)initWithContentType:(NSString *)contentType plugins:(NSString *)plugins;

@property (nonatomic) NSString *contentType;
@property (nonatomic) NSString *plugins;

@end


@implementation ContentTypeMap

- (instancetype)initWithContentType:(NSString *)contentType plugins:(NSString *)plugins {
    self = [super init];

    _contentType = contentType;
    _plugins = plugins;

    return self;
}

- (void)setContentType:(NSString *)contentType {
    _contentType = contentType;
    [NSNotificationCenter.defaultCenter postNotificationName:kContentTypeMappingChangedNotification object:self];
}

- (void)setPlugins:(NSString *)plugins {
    _plugins = plugins;
    [NSNotificationCenter.defaultCenter postNotificationName:kContentTypeMappingChangedNotification object:self];
}

@end

#pragma mark -

@interface NetworkPreferencesViewController ()

@property (nonatomic) BOOL enableNetworkProxy;
@property (nonatomic) NSString *networkProxyAddress;
@property (nonatomic) NSNumber *networkProxyPort;
@property (nonatomic) NSUInteger networkProxyType;
@property (nonatomic) NSString *networkProxyUserName;
@property (nonatomic) NSString *networkProxyPassword;
@property (nonatomic) NSString *networkProxyUserAgent;

@property (weak) IBOutlet NSArrayController *contentTypeMappingArrayController;
@property (weak) IBOutlet NSTableView *contentTypeMappingTableView;


@end

@implementation NetworkPreferencesViewController

- (void)viewDidLoad {
    [super viewDidLoad];

    // network
    _enableNetworkProxy = deadbeef->conf_get_int ("network.proxy", 0);
    [self willChangeValueForKey:@"enableNetworkProxy"];
    [self didChangeValueForKey:@"enableNetworkProxy"];

    _networkProxyAddress = [NSString stringWithUTF8String: deadbeef->conf_get_str_fast ("network.proxy.address", "")];
    [self willChangeValueForKey:@"networkProxyAddress"];
    [self didChangeValueForKey:@"networkProxyAddress"];

    _networkProxyPort = @(deadbeef->conf_get_int ("network.proxy.port", 8080));
    [self willChangeValueForKey:@"networkProxyPort"];
    [self didChangeValueForKey:@"networkProxyPort"];

    const char *type = deadbeef->conf_get_str_fast ("network.proxy.type", "HTTP");
    if (!strcasecmp (type, "HTTP")) {
        _networkProxyType = 0;
    }
    else if (!strcasecmp (type, "HTTP_1_0")) {
        _networkProxyType = 1;
    }
    else if (!strcasecmp (type, "SOCKS4")) {
        _networkProxyType = 2;
    }
    else if (!strcasecmp (type, "SOCKS5")) {
        _networkProxyType = 3;
    }
    else if (!strcasecmp (type, "SOCKS4A")) {
        _networkProxyType = 4;
    }
    else if (!strcasecmp (type, "SOCKS5_HOSTNAME")) {
        _networkProxyType = 5;
    }
    [self willChangeValueForKey:@"networkProxyType"];
    [self didChangeValueForKey:@"networkProxyType"];

    _networkProxyUserName = [NSString stringWithUTF8String:deadbeef->conf_get_str_fast ("network.proxy.username", "")];
    [self willChangeValueForKey:@"networkProxyUserName"];
    [self didChangeValueForKey:@"networkProxyUserName"];

    _networkProxyPassword = [NSString stringWithUTF8String:deadbeef->conf_get_str_fast ("network.proxy.password", "")];
    [self willChangeValueForKey:@"networkProxyPassword"];
    [self didChangeValueForKey:@"networkProxyPassword"];

    _networkProxyUserAgent = [NSString stringWithUTF8String:deadbeef->conf_get_str_fast ("network.http_user_agent", "")];
    [self willChangeValueForKey:@"networkProxyUserAgent"];
    [self didChangeValueForKey:@"networkProxyUserAgent"];

    // Content-type mapping
    [self initContentTypeMapping];

    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(contentTypeMappingChanged:) name:kContentTypeMappingChangedNotification object:nil];

}

- (void)contentTypeMappingChanged:(ContentTypeMap *)sender {
    NSArray<ContentTypeMap *> *objects = self.contentTypeMappingArrayController.arrangedObjects;

    char mapstr[2048] = "";
    int s = sizeof (mapstr);
    char *p = mapstr;

    for (ContentTypeMap *m in objects) {
        const char *ct = m.contentType ? m.contentType.UTF8String : "";
        const char *plugins = m.plugins ? m.plugins.UTF8String : "";
        int l = snprintf (p, s, "\"%s\" {%s} ", ct, plugins);
        p += l;
        s -= l;
        if (s <= 0) {
            break;
        }
    }

    deadbeef->conf_set_str ("network.ctmapping", mapstr);
    deadbeef->conf_save ();

    deadbeef->sendmessage (DB_EV_CONFIGCHANGED, 0, 0, 0);
}

- (IBAction)resetContentTypeMapping:(id)sender {
    deadbeef->conf_set_str ("network.ctmapping", DDB_DEFAULT_CTMAPPING);
    [self initContentTypeMapping];
}

- (void)initContentTypeMapping {
    [self.contentTypeMappingArrayController setContent:nil];

    char ctmap_str[2048];
    deadbeef->conf_get_str ("network.ctmapping", DDB_DEFAULT_CTMAPPING, ctmap_str, sizeof (ctmap_str));
    ddb_ctmap_t *ctmap = ddb_ctmap_init_from_string (ctmap_str);
    if (ctmap) {

        ddb_ctmap_t *m = ctmap;
        while (m) {
            NSString *plugins = @"";
            for (int i = 0; m->plugins[i]; i++) {
                if (i != 0) {
                    plugins = [plugins stringByAppendingString:@" "];
                }
                plugins = [plugins stringByAppendingString:[NSString stringWithUTF8String:m->plugins[i]]];
            }


            ContentTypeMap *map = [[ContentTypeMap alloc] initWithContentType:[NSString stringWithUTF8String:m->ct] plugins:plugins];
            [self.contentTypeMappingArrayController addObject:map];

            m = m->next;
        }

        ddb_ctmap_free (ctmap);
    }

    [self.contentTypeMappingTableView reloadData];
}

- (void)setEnableNetworkProxy:(BOOL)enableNetworkProxy {
    _enableNetworkProxy = enableNetworkProxy;
    deadbeef->conf_set_int ("network.proxy", enableNetworkProxy);
    deadbeef->sendmessage(DB_EV_CONFIGCHANGED, 0, 0, 0);
    deadbeef->conf_save ();
}

- (void)setNetworkProxyAddress:(NSString *)networkProxyAddress {
    _networkProxyAddress = networkProxyAddress;
    deadbeef->conf_set_str ("network.proxy.address", (networkProxyAddress?:@"").UTF8String);
    deadbeef->sendmessage(DB_EV_CONFIGCHANGED, 0, 0, 0);
    deadbeef->conf_save ();
}

- (void)setNetworkProxyPort:(NSNumber *)networkProxyPort {
    _networkProxyPort = networkProxyPort;
    deadbeef->conf_set_int ("network.proxy.port", (networkProxyPort?:@8080).unsignedIntValue);
    deadbeef->sendmessage(DB_EV_CONFIGCHANGED, 0, 0, 0);
    deadbeef->conf_save ();
}

- (void)setNetworkProxyType:(NSUInteger)networkProxyType {
    _networkProxyType = networkProxyType;
    const char *type = NULL;
    switch (networkProxyType) {
    case 0:
        type = "HTTP";
        break;
    case 1:
        type = "HTTP_1_0";
        break;
    case 2:
        type = "SOCKS4";
        break;
    case 3:
        type = "SOCKS5";
        break;
    case 4:
        type = "SOCKS4A";
        break;
    case 5:
        type = "SOCKS5_HOSTNAME";
        break;
    default:
        type = "HTTP";
        break;
    }

    deadbeef->conf_set_str ("network.proxy.type", type);
    deadbeef->sendmessage(DB_EV_CONFIGCHANGED, 0, 0, 0);
    deadbeef->conf_save ();
}

- (void)setNetworkProxyUserName:(NSString *)networkProxyUserName {
    _networkProxyUserName = networkProxyUserName;
    deadbeef->conf_set_str ("network.proxy.username", (networkProxyUserName?:@"").UTF8String);
    deadbeef->sendmessage(DB_EV_CONFIGCHANGED, 0, 0, 0);
    deadbeef->conf_save ();
}

- (void)setNetworkProxyPassword:(NSString *)networkProxyPassword {
    _networkProxyPassword = networkProxyPassword;
    deadbeef->conf_set_str ("network.proxy.password", (networkProxyPassword?:@"").UTF8String);
    deadbeef->sendmessage(DB_EV_CONFIGCHANGED, 0, 0, 0);
    deadbeef->conf_save ();
}

- (void)setNetworkProxyUserAgent:(NSString *)networkProxyUserAgent {
    _networkProxyUserAgent = networkProxyUserAgent;
    deadbeef->conf_set_str ("network.http_user_agent", (networkProxyUserAgent?:@"").UTF8String);
    deadbeef->sendmessage(DB_EV_CONFIGCHANGED, 0, 0, 0);
    deadbeef->conf_save ();
}



@end