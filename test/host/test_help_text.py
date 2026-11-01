"""The help text is the only documentation a user has. Three of its claims were true of the
computed-image firmware and are false since the card is the machine's disk directly."""
import sys, pathlib

src = pathlib.Path(sys.argv[1], 'components/setupfile/help_text.c').read_text(encoding='utf-8')

# The setup AP lost its "-Setup-" infix; the help must not name the old one.
assert 'SilbapStick-Setup-' not in src, 'the setup AP is SilbapStick-XXXX now'

# The machine reads the card itself: no size ceiling, no hidden names, no romanisation for it.
for wrong in ('2GB 이상 파일과', 'are not shown to the machine', '不显示给机器', 'ミシンに表示されません'):
    assert wrong not in src, f'stale claim survived: {wrong}'
for wrong in ('기계에 보이는 이름은 로마자로 바뀌어요', 'Names shown on the machine are romanised',
              '机器上显示的名称会转为罗马字', 'ミシンに表示される名前はローマ字になります'):
    assert wrong not in src, f'stale romanisation claim survived: {wrong}'

# Every language carries all four sections exactly once.
for tag in ('[ko]', '[en]', '[zh]', '[ja]'):
    assert src.count(tag) == 1, f'missing or duplicated section {tag}'

# The approved wording, verbatim, one item per language: a paraphrase is a regression too.
# The new AP name is shared by all four languages, so it appears once per section, not once.
assert src.count('SilbapStick-XXXX') == 4, 'the new setup AP name must appear in all four sections'

APPROVED = {
    'ko': [
        '8) 동글 화면에 나온 주소를 브라우저에 넣으면 파일을 올리고 지울 수 있어요. 미싱은 SD 카드를 직접 읽으므로 파일 이름과 크기에 제한이 없어요. 숨김 처리는 웹 목록에만 적용돼요.',
        '9) 한 폴더에 파일은 수백 개 이하로 두세요. 그래야 미싱 화면이 빨라요. 웹 목록의 이름은 로마자로 바꿔 볼 수 있지만(설정에서 끌 수 있어요), 미싱에 보이는 이름은 바뀌지 않아요.',
    ],
    'en': [
        '8) Put the address from the dongle screen in a browser to upload and delete files. The machine reads the SD card directly, so no name or size is filtered for it; the hide rule applies to the web listing only.',
        '9) Keep at most a few hundred files per folder so the machine\'s own screen stays fast. The web listing can show romanised names to help you find a file (you can turn that off in settings); the name the machine shows never changes.',
    ],
    'zh': [
        '8) 在浏览器输入U盘屏幕上显示的地址即可上传和删除文件。机器直接读取SD卡，因此文件名和大小不受限制；隐藏规则仅适用于网页列表。',
        '9) 每个文件夹最好不超过几百个文件，机器屏幕才会流畅。网页列表可以显示罗马字名称以便查找（可在设置中关闭），但机器显示的名称不会改变。',
    ],
    'ja': [
        '8) ドングルの画面のアドレスをブラウザに入力すると、ファイルのアップロードと削除ができます。ミシンはSDカードを直接読むので、名前やサイズによる制限はありません。非表示の規則はWebの一覧にのみ適用されます。',
        '9) 1フォルダのファイルは数百個までにすると、ミシン本体の画面が速くなります。Webの一覧はローマ字表示にできますが（設定でオフにできます）、ミシンに表示される名前は変わりません。',
    ],
}
for lang, lines in APPROVED.items():
    for line in lines:
        assert src.count(line) == 1, f'[{lang}] missing or repeated: {line[:40]}'
