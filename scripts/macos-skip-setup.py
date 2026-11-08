#!/usr/bin/env python3
"""Skip the macOS first-boot setup on the full-OS Data volume (run as root on the mounted Data volume).

Writes what loginwindow checks before it would run Language Chooser and Setup Assistant (the same approach as
Droid-VM/macOS-DroidVM scripts/stage-gpu-plugin.py), plus auto-login and the Spanish locale:
  private/var/db/.AppleSetupDone                                          setup done: go to the login window
  Library/Managed Preferences/com.apple.SetupAssistant.managed.plist      SkipSetupItems for the per-user assistant
  private/var/db/dslocal/nodes/Default/users/<name>.plist                 local admin user (PBKDF2-SHA512 password)
  private/var/db/dslocal/nodes/Default/groups/{admin,staff}.plist         membership
  Library/Preferences/com.apple.loginwindow.plist + private/etc/kcpassword auto-login (the tablet has no keyboard)
  Library/Preferences/.GlobalPreferences.plist                            AppleLanguages / AppleLocale
  private/var/db/com.apple.xpc.launchd/disabled.plist                     jobs that wait for missing Apple hardware
  Users/<name> (from Library/User Template), accountPolicyData, per-user SetupAssistant prefs: what Setup Assistant
                                                                          makes and the record alone lacks
Usage: sudo python3 macos-skip-setup.py <Data mount point> [user] [password]
The mount point must be the Data volume in ipsw/macos26/fullos/data-uuid.txt; it is checked before anything is written.
"""
import hashlib
import os
import plistlib
import subprocess
import sys
import time
import uuid

# The Data volume made for the current image (fullos-volumes.sh writes it; 991CB5AC… was the 2026-11-02 one).
D = os.environ.get('UKE_ROOT') or os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
DATA_UUID = open(D + '/ipsw/macos26/fullos/data-uuid.txt').read().strip()
OS_BUILD, OS_VERSION = '25F71', '26.5'   # the full-OS image (System/Library/CoreServices/SystemVersion.plist)
SKIP_ITEMS = ['Accessibility', 'AdditionalPrivacySettings', 'Appearance', 'AppleID', 'AppStore', 'Biometric',
              'Diagnostics', 'DisplayTone', 'EnableLockdownMode', 'FileVault', 'iCloudDiagnostics', 'iCloudStorage',
              'Intelligence', 'Location', 'OSShowcase', 'Privacy', 'Restore', 'Safety', 'ScreenTime', 'Siri',
              'SoftwareUpdate', 'TermsOfAddress', 'TextSize', 'TOS', 'TouchIDPrompt', 'UnlockWithWatch', 'Wallpaper',
              'Welcome']
DISABLED_JOBS = ['com.apple.mobileactivationd', 'com.apple.watchdogd']
KCPASSWORD_KEY = [0x7D, 0x89, 0x52, 0x23, 0xD2, 0xBC, 0xDD, 0xEA, 0xA3, 0xB9, 0x1F]


def write_plist(path, value, mode=0o644, binary=True):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        plistlib.dump(value, f, fmt=plistlib.FMT_BINARY if binary else plistlib.FMT_XML)
    os.chown(path, 0, 0)
    os.chmod(path, mode)


def update_plist(path, change, mode=0o644):
    value = {}
    if os.path.exists(path):
        with open(path, 'rb') as f:
            value = plistlib.load(f)
    change(value)
    write_plist(path, value, mode)


def shadow_hash(password):
    salt, iterations = os.urandom(32), 50000
    entropy = hashlib.pbkdf2_hmac('sha512', password.encode(), salt, iterations, 128)
    return plistlib.dumps({'SALTED-SHA512-PBKDF2': {'entropy': entropy, 'iterations': iterations, 'salt': salt}},
                          fmt=plistlib.FMT_BINARY)


def kcpassword(password):
    data = bytearray(password.encode())
    data += bytes(12 - len(data) % 12)
    return bytes(b ^ KCPASSWORD_KEY[i % len(KCPASSWORD_KEY)] for i, b in enumerate(data))


def main():
    if os.geteuid() != 0 or len(sys.argv) < 2:
        sys.exit(__doc__)
    data = os.path.realpath(sys.argv[1])
    name = sys.argv[2] if len(sys.argv) > 2 else 'uke'
    password = sys.argv[3] if len(sys.argv) > 3 else 'uke'
    info = subprocess.run(['diskutil', 'info', data], capture_output=True, text=True).stdout
    if DATA_UUID not in info or 'Volume Name:               Data' not in info:
        sys.exit(f'{data} is not the full-OS Data volume {DATA_UUID}')
    subprocess.run(['diskutil', 'enableOwnership', data], check=True)

    db = os.path.join(data, 'private/var/db')
    marker = os.path.join(db, '.AppleSetupDone')
    open(marker, 'wb').close()
    os.chown(marker, 0, 0)
    os.chmod(marker, 0o444)

    write_plist(os.path.join(data, 'Library/Managed Preferences/com.apple.SetupAssistant.managed.plist'),
                {'SkipSetupItems': SKIP_ITEMS}, binary=False)

    nodes = os.path.join(db, 'dslocal/nodes/Default')
    users = os.path.join(nodes, 'users')
    record_path = os.path.join(users, name + '.plist')
    old = {}
    if os.path.exists(record_path):   # rerun: keep the same uid and GUID (group membership refers to them)
        with open(record_path, 'rb') as f:
            old = plistlib.load(f)
    guid = old.get('generateduid', [str(uuid.uuid4()).upper()])[0]
    uid = int(old['uid'][0]) if 'uid' in old else 501
    if not old:
        taken = set()
        for entry in os.listdir(users):
            with open(os.path.join(users, entry), 'rb') as f:
                taken.update(int(u) for u in plistlib.load(f).get('uid', []))
        while uid in taken:
            uid += 1
    now = time.time()
    policy = plistlib.dumps({'creationTime': now, 'failedLoginCount': 0, 'failedLoginTimestamp': 0,
                             'passwordLastSetTime': now}, fmt=plistlib.FMT_XML)
    write_plist(record_path, {
        'name': [name], 'realname': [name], 'uid': [str(uid)], 'gid': ['20'], 'home': ['/Users/' + name],
        'shell': ['/bin/zsh'], 'generateduid': [guid], 'passwd': ['********'],
        'authentication_authority': [';ShadowHash;HASHLIST:<SALTED-SHA512-PBKDF2>'],
        'ShadowHashData': [shadow_hash(password)], 'accountPolicyData': [policy],
        '_writers_passwd': [name], '_writers_realname': [name], '_writers_hint': [name]}, mode=0o600)

    # Home folder from the User Template, as Setup Assistant makes it (the record alone leaves /Users/<name> missing).
    home = os.path.join(data, 'Users', name)
    template = os.path.join(data, 'Library/User Template')
    if not os.path.isdir(home):
        subprocess.run(['ditto', os.path.join(template, 'Non_localized'), home], check=True)
        subprocess.run(['ditto', os.path.join(template, 'Spanish.lproj'), home], check=True)
        subprocess.run(['chown', '-R', f'{uid}:20', home], check=True)
        os.chmod(home, 0o750)
        os.chmod(os.path.join(home, 'Public'), 0o755)
        os.chmod(os.path.join(home, 'Public/Drop Box'), 0o733)
    # Per-user assistant (MiniBuddy) already seen.
    buddy = os.path.join(home, 'Library/Preferences/com.apple.SetupAssistant.plist')
    # The DidSee* flags alone are not enough: without the version keys loginwindow treats the first login as an upgrade
    # install and launches MiniBuddy (Setup Assistant in the user session), and the Dock and menu bar wait for it
    # (2026-11-06: desktop and widgets drawn, no Dock, no menu bar). Keys from the 25F71 Setup Assistant binary, values
    # as a Mac set up on this build would have them.
    update_plist(buddy, lambda p: p.update(
        DidSeeCloudSetup=True, DidSeePrivacy=True, DidSeeSiriSetup=True, DidSeeScreenTime=True,
        DidSeeAppearanceSetup=True, DidSeeTouchIDSetup=True, DidSeeAccessibility=True, DidSeeActivationLock=True,
        DidSeeSyncSetup=True, DidSeeSyncSetup2=True, DidSeeIntelligence=True, DidSeeAppStore=True,
        DidSeeApplePaySetup=True, DidSeeLockdownMode=True, DidSeeTermsOfAddress=True,
        DidSeeiCloudLoginForStorageServices=True, DidSeeNewFeaturesProductVersion=OS_VERSION,
        InitialSetupBuildVersion=OS_BUILD, InitialSetupProductVersion=OS_VERSION,
        LastSeenBuddyBuildVersion=OS_BUILD, PreviousBuildVersion=OS_BUILD, PreviousSystemVersion=OS_VERSION,
        LastPreLoginTasksPerformedBuild=OS_BUILD, LastPreLoginTasksPerformedVersion=OS_VERSION,
        LastSeenCloudProductVersion=OS_VERSION, LastSeenDiagnosticsProductVersion=OS_VERSION,
        LastSeenIntelligenceProductVersion=OS_VERSION, LastSeenSiriProductVersion=OS_VERSION,
        LastSeenSyncProductVersion=OS_VERSION, LastSeeniCloudStorageServicesProductVersion=OS_VERSION,
        LastSeenAgeRangeSelectionProductVersion=OS_VERSION, LastLaunchedMiniBuddyForStorageServicesVersion=OS_VERSION,
        LastPrivacyBundleVersion=2, MiniBuddyLaunchReason=0, MiniBuddyLaunchedPostMigration=False,
        MiniBuddyShouldLaunchToResumeSetup=False, SkipFirstLoginOptimization=True, InitialAccountOnMac=True,
        GPOnboardingHasShown=True))
    os.chown(buddy, uid, 20)
    for group in ('admin', 'staff'):
        def join(record):
            record['users'] = [u for u in record.get('users', []) if u != name] + [name]
            record['groupmembers'] = [g for g in record.get('groupmembers', []) if g != guid] + [guid]
        update_plist(os.path.join(nodes, 'groups', group + '.plist'), join, mode=0o600)

    update_plist(os.path.join(data, 'Library/Preferences/com.apple.loginwindow.plist'),
                 lambda p: p.update(autoLoginUser=name, OptimizerPreviousBuild=OS_BUILD, lastUserName=name,
                                    RecentUsers=[name], AccountInfo={'FirstLogins': {name: True}, 'MaximumUsers': 1,
                                                                     'OnConsole': {}}))
    kc = os.path.join(data, 'private/etc/kcpassword')
    with open(kc, 'wb') as f:
        f.write(kcpassword(password))
    os.chown(kc, 0, 0)
    os.chmod(kc, 0o600)

    update_plist(os.path.join(data, 'Library/Preferences/.GlobalPreferences.plist'),
                 lambda p: p.update(AppleLanguages=['es-ES', 'en'], AppleLocale='es_ES'))

    update_plist(os.path.join(db, 'com.apple.xpc.launchd/disabled.plist'),
                 lambda p: p.update({job: True for job in DISABLED_JOBS}))

    print(f'setup skipped: user {name} (uid {uid}, {guid}), auto-login, es_ES, disabled {", ".join(DISABLED_JOBS)}')


if __name__ == '__main__':
    main()
