#include "setupfile.h"
#include <stdio.h>
#include <string.h>

static const char *const k_sections =
"\n[ko]\n"
"1) 접속: 동글 화면(LCD)에 나온 IP 주소를 브라우저에 입력해요. PC와 아이폰은 http://silbapstick.local 도 돼요.\n"
"2) 처음 Wi-Fi 설정: 휴대폰이나 PC의 Wi-Fi 목록에서 SilbapStick-Setup-XXXX에 연결해요. 비밀번호는 동글 화면에 나와요. 연결되면 설정 페이지가 열려요.\n"
"3) 비밀번호를 잊었을 때: 동작 중에 BOOT 버튼을 5초 누르세요. 또는 SD 카드의 silbap-setup.txt에 web_password= 한 줄을 넣고 다시 꽂으세요.\n"
"4) 완전 초기화: BOOT 버튼을 15초 누르세요. Wi-Fi와 설정이 지워져요. 파일은 그대로예요.\n"
"5) 설정 파일 silbap-setup.txt: wifi_ssid, wifi_password(8자 이상), web_password, rename(on/off), language(ko/en/zh/ja). 켤 때 적용되고 silbap-setup.done.txt로 이름이 바뀌어요. 비밀번호는 가려져요.\n"
"6) SD 카드는 FAT32여야 해요(exFAT 안 돼요). Windows: 32GB를 넘는 카드는 FAT32 포맷 도구를 쓰세요. Mac: 디스크 유틸리티에서 MS-DOS(FAT)로 지우세요.\n"
"7) Wi-Fi는 2.4GHz만 돼요. 게스트 네트워크나 기기 격리(AP isolation)가 켜져 있으면 접속이 막혀요. 공유기에서 IP를 고정해 두면 편해요.\n"
"8) 한 폴더에 파일은 수백 개 이하로 두세요. 그래야 재봉틀 화면이 빨라요. 2GB 이상 파일과 이름이 .으로 시작하는 파일은 기계에 보이지 않아요.\n"
"9) 기계에 보이는 이름은 로마자로 바뀌어요(한국어는 국어의 로마자 표기법, 중국어는 병음, 일본어는 헵번식). 설정에서 끌 수 있어요.\n"
"\n[en]\n"
"1) Open the page: type the IP address shown on the dongle LCD into a browser. On PCs and iPhones http://silbapstick.local also works.\n"
"2) First Wi-Fi setup: join SilbapStick-Setup-XXXX from your phone or PC. The password is shown on the dongle screen. The setup page then opens.\n"
"3) Forgot the password: hold BOOT for 5 s while it is running. Or put a line web_password= in silbap-setup.txt on the SD card and plug it in again.\n"
"4) Full reset: hold BOOT for 15 s. Wi-Fi and settings are erased; your files are kept.\n"
"5) Setup file silbap-setup.txt keys: wifi_ssid, wifi_password (at least 8 characters), web_password, rename (on/off), language (ko/en/zh/ja). They are applied at boot, then the file is renamed to silbap-setup.done.txt with passwords hidden.\n"
"6) The SD card must be FAT32 (exFAT is not supported). Windows: for cards over 32 GB use a FAT32 format tool. Mac: Disk Utility, format MS-DOS (FAT).\n"
"7) Wi-Fi is 2.4 GHz only. Guest networks and client isolation (AP isolation) block access. Reserve the IP in your router so it does not change.\n"
"8) Keep at most a few hundred files per folder so the machine screen stays fast. Files of 2 GB or more, and names starting with \".\", are not shown to the machine.\n"
"9) Names shown on the machine are romanised (Korean: Revised Romanization, Chinese: pinyin, Japanese: Hepburn). You can turn this off in settings.\n"
"\n[zh]\n"
"1) 打开网页：在浏览器输入U盘屏幕(LCD)上显示的IP地址。电脑和iPhone也可使用 http://silbapstick.local 。\n"
"2) 首次设置Wi-Fi：用手机或电脑连接 SilbapStick-Setup-XXXX，密码显示在U盘屏幕上，连接后会自动打开设置页面。\n"
"3) 忘记密码：运行时按住BOOT键5秒；或在SD卡的 silbap-setup.txt 中加入一行 web_password= ，然后重新插入。\n"
"4) 完全重置：按住BOOT键15秒。Wi-Fi和设置会被清除，文件保留。\n"
"5) 设置文件 silbap-setup.txt 的键：wifi_ssid、wifi_password(至少8个字符)、web_password、rename(on/off)、language(ko/en/zh/ja)。启动时应用，并改名为 silbap-setup.done.txt，密码会被隐藏。\n"
"6) SD卡必须是FAT32（不支持exFAT）。Windows：超过32GB的卡请用FAT32格式化工具。Mac：磁盘工具中选择 MS-DOS (FAT)。\n"
"7) 仅支持2.4GHz Wi-Fi。访客网络和设备隔离(AP isolation)会阻止访问。建议在路由器中保留(固定)该IP。\n"
"8) 每个文件夹最好不超过几百个文件，机器屏幕才会流畅。2GB及以上的文件和以“.”开头的名称不会显示给机器。\n"
"9) 机器上显示的名称会转为罗马字（韩语：修订罗马化，中文：拼音，日语：平文式）。可在设置中关闭。\n"
"\n[ja]\n"
"1) 開き方：ドングルの画面(LCD)に表示されたIPアドレスをブラウザに入力します。PCとiPhoneでは http://silbapstick.local も使えます。\n"
"2) 最初のWi-Fi設定：スマホやPCから SilbapStick-Setup-XXXX に接続します。パスワードはドングルの画面に表示されます。接続すると設定ページが開きます。\n"
"3) パスワードを忘れたとき：動作中にBOOTボタンを5秒押します。またはSDカードの silbap-setup.txt に web_password= の行を入れて、差し直します。\n"
"4) 完全リセット：BOOTボタンを15秒押します。Wi-Fiと設定が消えます。ファイルは残ります。\n"
"5) 設定ファイル silbap-setup.txt のキー：wifi_ssid、wifi_password(8文字以上)、web_password、rename(on/off)、language(ko/en/zh/ja)。起動時に反映され、パスワードを隠して silbap-setup.done.txt に名前が変わります。\n"
"6) SDカードはFAT32が必要です(exFAT不可)。Windows：32GBを超えるカードはFAT32フォーマットツールを使います。Mac：ディスクユーティリティで MS-DOS (FAT) を選びます。\n"
"7) Wi-Fiは2.4GHzのみです。ゲストネットワークや端末分離(AP isolation)があるとアクセスできません。ルーターでIPを固定すると便利です。\n"
"8) 1フォルダのファイルは数百個までにすると、ミシンの画面が速くなります。2GB以上のファイルと「.」で始まる名前はミシンに表示されません。\n"
"9) ミシンに表示される名前はローマ字になります(韓国語は改定ローマ字、中国語は拼音、日本語はヘボン式)。設定でオフにできます。\n";

static const char *skip_v(const char *v) { return (v && *v == 'v') ? v + 1 : (v ? v : ""); }

int sf_help_text(const char *version, char *out, size_t out_sz) {
    if (!out || !out_sz) return -1;
    int n = snprintf(out, out_sz, "SilbapRemoteStick help v%s\n%s", skip_v(version), k_sections);
    return (n < 0 || (size_t)n >= out_sz) ? -1 : n;
}

bool sf_help_is_current(const char *existing_first_line, const char *version) {
    if (!existing_first_line) return false;
    char want[96];
    int n = snprintf(want, sizeof want, "SilbapRemoteStick help v%s", skip_v(version));
    if (n < 0 || (size_t)n >= sizeof want) return false;
    size_t wl = (size_t)n;
    if (strncmp(existing_first_line, want, wl)) return false;
    const char *r = existing_first_line + wl;
    return *r == 0 || *r == '\n' || (*r == '\r' && (r[1] == 0 || r[1] == '\n'));
}
