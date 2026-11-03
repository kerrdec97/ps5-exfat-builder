"""Interface languages (issues #23 / #35: sidebar stayed English)."""
import unittest

from tests._support import load_app

NAV = ['Home', 'Build', 'Extract', 'Edit', 'Library', 'Convert',
       'Dump Rename', 'Backports', 'Advanced', 'History', 'Settings',
       'Credits', 'Report Issue', 'WORKFLOWS', 'MAIN TOOLS', 'ADVANCED']
LANGS = ['zh', 'de', 'fr', 'es', 'pt', 'ja', 'ko', 'ru', 'ar', 'it', 'nl',
         'pl', 'tr', 'th', 'vi', 'id']


class Translations(unittest.TestCase):
    def setUp(self):
        self.app = load_app()
        self.addCleanup(self.app.set_language, 'en')

    def test_seventeen_languages(self):
        self.assertEqual(sorted(self.app._TRANSLATIONS), sorted(LANGS))
        self.assertEqual(len(LANGS) + 1, 17)   # + English source strings

    def test_every_navigation_label_is_translated(self):
        for lang in LANGS:
            table = self.app._TRANSLATIONS[lang]
            missing = [k for k in NAV if not table.get(k)]
            self.assertEqual(missing, [], lang)

    def test_existing_community_strings_were_not_overwritten(self):
        t = self.app._TRANSLATIONS
        self.assertEqual(t['zh']['Extract'], '提取')
        self.assertEqual(t['de']['Settings'], 'Einstellungen')
        self.assertEqual(t['fr']['Library'], 'Bibliothèque')

    def test_switching_language_changes_lookup(self):
        app = self.app
        app.set_language('de')
        self.assertEqual(app._('Build'), 'Erstellen')
        app.set_language('zh')
        self.assertEqual(app._('Home'), '主页')
        app.set_language('en')
        self.assertEqual(app._('Build'), 'Build')
        app.set_language('de')
        self.assertEqual(app._('a string nobody translated'),
                         'a string nobody translated')

    def test_ui_modules_share_the_main_module_language(self):
        # The v3.6.x bug behind #35: ui/* imported a *second* copy of the
        # main module, so Apply changed the language in the wrong place.
        import importlib
        app = self.app
        tab_language = importlib.import_module('ui.tab_language')
        self.assertIs(tab_language.set_language, app.set_language)
        tab_language.set_language('fr')
        self.assertEqual(app._('Home'), 'Accueil')

    def test_header_section_names_are_translatable(self):
        app = self.app
        for key, en in app.ExFATBuilder._HEADER_SECTION_NAMES.items():
            if en == 'PS5':
                continue
            self.assertIn(en, self.app._TRANSLATIONS['de'], key)


if __name__ == '__main__':
    unittest.main()
