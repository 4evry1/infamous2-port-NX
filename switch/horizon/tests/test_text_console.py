from pathlib import Path
import re
import subprocess
import tempfile
import unittest


class TextConsole(unittest.TestCase):
    def test_owned_console_lifecycle(self):
        source = (Path(__file__).parent.parent / 'src/hz_entry.c').read_text()
        functions = []
        for name in ('text_console_start', 'text_console_refresh', 'text_console_stop'):
            match = re.search(r'static void ' + name + r'\(void\)\n\{.*?\n\}', source, re.S)
            self.assertIsNotNone(match, name)
            functions.append(match.group())
        fixture = '''
#include <assert.h>
#include <stddef.h>
static int s_text_console_enabled = 1, s_screen_owned;
static int started, refreshed, stopped;
static void consoleInit(void *unused) { (void)unused; assert(!s_screen_owned); ++started; }
static void consoleUpdate(void *unused) { (void)unused; assert(s_screen_owned); ++refreshed; }
static void consoleExit(void *unused) { (void)unused; assert(s_screen_owned); ++stopped; }
'''
        fixture += '\n'.join(functions)
        fixture += '''
int main(void) {
    text_console_refresh(); text_console_stop();
    assert(started == 0 && refreshed == 0 && stopped == 0);
    text_console_start(); text_console_start(); text_console_refresh();
    assert(started == 1 && refreshed == 1 && s_screen_owned);
    text_console_stop(); text_console_stop(); text_console_refresh();
    assert(stopped == 1 && !s_screen_owned && refreshed == 1);
    s_text_console_enabled = 0;
    text_console_start(); text_console_refresh(); text_console_stop();
    assert(started == 1 && refreshed == 1 && stopped == 1 && !s_screen_owned);
    s_text_console_enabled = 1;
    text_console_start(); text_console_refresh(); text_console_stop();
    assert(started == 2 && refreshed == 2 && stopped == 2 && !s_screen_owned);
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            (path / 'test.c').write_text(fixture)
            subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined',
                            str(path / 'test.c'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)

    def test_console_calls_stay_inside_owned_helpers(self):
        source = (Path(__file__).parent.parent / 'src/hz_entry.c').read_text()
        for api in ('consoleInit', 'consoleUpdate', 'consoleExit'):
            self.assertEqual(source.count(api + '('), 1, api)
        self.assertIn('env_number("HZ_TEXT_CONSOLE", 1)', source)


if __name__ == '__main__':
    unittest.main()
