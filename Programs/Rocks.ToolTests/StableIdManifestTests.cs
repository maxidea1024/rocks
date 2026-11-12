using Rocks.MetaCompiler;

namespace Rocks.ToolTests;

internal static class StableIdManifestTests
{
    public static void Register(TestSuite suite)
    {
        suite.Add("StableIds.RegistrationRenameRetireAndReadOnlyBuild", () =>
        {
            using TemporaryDirectory temporary = new();
            string path = temporary.FilePath("identity.json");
            string[] ids = StableIdManifest.Update(path, true, manifest =>
            {
                string type = manifest.Register("type", "sample::Character");
                string field = manifest.Register("field", "sample::Character::health");
                return new[] { type, field };
            });
            byte[] beforeRead = File.ReadAllBytes(path);
            StableIdManifest read = StableIdManifest.Load(path);
            Verify.Equal(ids[0], read.Resolve("type", "sample::Character"), "type registration");
            Verify.Equal(ids[1], read.Resolve("field", "sample::Character::health"), "field registration");
            Verify.That(beforeRead.SequenceEqual(File.ReadAllBytes(path)), "build lookup changed manifest");

            StableIdManifest.Update(path, false, manifest =>
            {
                manifest.Rename("type", "sample::Character", "sample::Player");
                return 0;
            });
            read = StableIdManifest.Load(path);
            Verify.Equal(ids[0], read.Resolve("type", "sample::Player"), "type rename lost ID");
            Verify.Equal(ids[1], read.Resolve("field", "sample::Player::health"), "type rename lost field ID");
            Verify.Throws<InvalidDataException>(() => read.Resolve("type", "sample::Character"));
            Verify.Throws<InvalidDataException>(() => read.Resolve("field", "sample::Character::health"));
            Verify.That(read.Entries.Single(e => e.Id == ids[1]).Aliases.Contains("sample::Character::health"), "field history missing");

            StableIdManifest.Update(path, false, manifest =>
            {
                manifest.Rename("field", "sample::Player::health", "sample::Player::hitpoints");
                return 0;
            });
            read = StableIdManifest.Load(path);
            Verify.Equal(ids[1], read.Resolve("field", "sample::Player::hitpoints"), "field rename lost ID");
            Verify.Throws<InvalidDataException>(() => StableIdManifest.Update(path, false, manifest => manifest.Register("field", "sample::Player::health")));
            StableIdManifest.Update(path, false, manifest =>
            {
                manifest.Retire("type", "sample::Player");
                return 0;
            });
            read = StableIdManifest.Load(path);
            Verify.Throws<InvalidDataException>(() => read.Resolve("type", "sample::Player"));
            Verify.Throws<InvalidDataException>(() => read.Resolve("field", "sample::Player::hitpoints"));
            Verify.Throws<InvalidDataException>(() => StableIdManifest.Update(path, false, manifest => manifest.Register("type", "sample::Character")));
        });

        suite.Add("StableIds.FailedMutationsPreserveOriginalBytes", () =>
        {
            using TemporaryDirectory temporary = new();
            string path = temporary.FilePath("identity.json");
            string[] ids = StableIdManifest.Update(path, true, manifest =>
            {
                string type = manifest.Register("type", "sample::GameObject");
                string field = manifest.Register("field", "sample::GameObject::health");
                return new[] { type, field };
            });
            Verify.Equal(ids[0], StableIdManifest.Load(path).Resolve("type", "sample::GameObject"), "type registration");
            Verify.Equal(ids[1], StableIdManifest.Load(path).Resolve("field", "sample::GameObject::health"), "field registration");
            byte[] before = File.ReadAllBytes(path);
            Verify.Throws<InvalidDataException>(() => StableIdManifest.Update(path, false, m => m.Register("type", "sample::GameObject")));
            Verify.Throws<InvalidDataException>(() => StableIdManifest.Update(path, false, m =>
            {
                m.Rename("type", "unknown", "unknown");
                return 0;
            }));
            Verify.Throws<InvalidDataException>(() => StableIdManifest.Update(path, false, m => m.Register("field", "missing::field")));
            Verify.Throws<InvalidDataException>(() => StableIdManifest.Update(path, false, m =>
            {
                m.Rename("field", "sample::GameObject::health", "other::field");
                return 0;
            }));
            Verify.Throws<InvalidOperationException>(() => StableIdManifest.Update<int>(path, false, m =>
            {
                m.Register("type", "sample::Temporary");
                throw new InvalidOperationException("injected");
            }));
            Verify.That(before.SequenceEqual(File.ReadAllBytes(path)), "failed mutations modified manifest");
            Verify.That(!File.Exists(path + ".lock"), "failed mutation left lock");
        });

        suite.Add("StableIds.StrictManifestRejectsMergeDamageAndAmbiguousHistory", () =>
        {
            using TemporaryDirectory temporary = new();
            string path = temporary.FilePath("identity.json");
            void Reject(string content)
            {
                File.WriteAllText(path, content);
                Verify.Throws<InvalidDataException>(() => StableIdManifest.Load(path));
            }

            Reject("<<<<<<< branch\n=======\n>>>>>>> branch\n");
            Reject("{\"schema\":1,\"schema\":1,\"entries\":[]}");
            Reject("{\"schema\":1,\"unknown\":1,\"entries\":[]}");
            Reject("{\"schema\":2,\"entries\":[]}");
            Reject("{\"schema\":1,\"entries\":[{\"kind\":\"type\",\"name\":\"A\",\"id\":\"00000000000000000000000000000000\",\"aliases\":[],\"retired\":false}]}");
            Reject("{\"schema\":1,\"entries\":[{\"kind\":\"type\",\"name\":\"A\",\"id\":\"11111111111111111111111111111111\",\"aliases\":[\"B\"],\"retired\":false},{\"kind\":\"type\",\"name\":\"B\",\"id\":\"22222222222222222222222222222222\",\"aliases\":[],\"retired\":false}]}");
            Reject("{\"schema\":1,\"entries\":[{\"kind\":\"type\",\"name\":\"A\",\"id\":\"11111111111111111111111111111111\",\"aliases\":[],\"retired\":false},{\"kind\":\"type\",\"name\":\"B\",\"id\":\"11111111111111111111111111111111\",\"aliases\":[],\"retired\":true}]}");
            Reject("{\"schema\":1,\"entries\":[{\"kind\":\"field\",\"name\":\"A::x\",\"id\":\"11111111111111111111111111111111\",\"aliases\":[],\"retired\":false}]}");
            Reject("{\"schema\":1,\"entries\":[{\"kind\":\"type\",\"name\":\"A\",\"id\":\"11111111111111111111111111111111\",\"aliases\":[],\"retired\":false},{\"kind\":\"type\",\"name\":\"B\",\"id\":\"22222222222222222222222222222222\",\"aliases\":[],\"retired\":false},{\"kind\":\"field\",\"name\":\"A::x\",\"id\":\"33333333333333333333333333333333\",\"aliases\":[\"B::x\"],\"retired\":false}]}");
        });

        suite.Add("StableIds.LockAndExternalMutationFailClosed", () =>
        {
            using TemporaryDirectory temporary = new();
            string path = temporary.FilePath("identity.json");
            StableIdManifest.Update(path, true, manifest => manifest.Register("type", "A"));
            byte[] before = File.ReadAllBytes(path);
            using (new FileStream(path + ".lock", FileMode.CreateNew, FileAccess.Write, FileShare.None))
            {
                Verify.Throws<IOException>(() => StableIdManifest.Update(path, false, m => m.Register("type", "B")));
            }

            File.Delete(path + ".lock");
            Verify.That(before.SequenceEqual(File.ReadAllBytes(path)), "lock conflict changed manifest");
            Verify.Throws<IOException>(() => StableIdManifest.Update(path, false, m =>
            {
                m.Register("type", "B");
                File.WriteAllText(path, "external edit");
                return 0;
            }));
            Verify.Equal("external edit", File.ReadAllText(path), "external edit was overwritten");
        });

        suite.Add("StableIds.SemanticNoOpDoesNotRewriteManifest", () =>
        {
            using TemporaryDirectory temporary = new();
            string path = temporary.FilePath("identity.json");
            const string compact = "{\"schema\":1,\"entries\":[]}";
            File.WriteAllText(path, compact);
            StableIdManifest.Update(path, false, _ => 0);
            Verify.Equal(compact, File.ReadAllText(path), "unchanged manifest was rewritten");
        });

        suite.Add("StableIds.RetiredFieldsFollowOwnerRenameWithoutReusingHistory", () =>
        {
            using TemporaryDirectory temporary = new();
            string path = temporary.FilePath("identity.json");
            string[] ids = StableIdManifest.Update(path, true, manifest =>
            {
                string type = manifest.Register("type", "old::Actor");
                string retired = manifest.Register("field", "old::Actor::obsolete");
                string live = manifest.Register("field", "old::Actor::health");
                manifest.Register("type", "other::Actor");
                manifest.Register("field", "other::Actor::obsolete");
                manifest.Retire("field", "old::Actor::obsolete");
                return new[] { type, retired, live };
            });
            StableIdManifest.Update(path, false, manifest =>
            {
                manifest.Rename("type", "old::Actor", "newscope::Actor");
                return 0;
            });
            StableIdManifest current = StableIdManifest.Load(path);
            StableIdEntry retiredField = current.Entries.Single(entry => entry.Id == ids[1]);
            Verify.That(retiredField.Retired && retiredField.Name == "newscope::Actor::obsolete" &&
                retiredField.Aliases.Contains("old::Actor::obsolete"), "retired field rename history lost");
            Verify.Equal(ids[2], current.Resolve("field", "newscope::Actor::health"), "active field identity lost");
            Verify.Throws<InvalidDataException>(() => StableIdManifest.Update(path, false, m => m.Register("field", "newscope::Actor::obsolete")));
            Verify.Throws<InvalidDataException>(() => StableIdManifest.Update(path, false, m => m.Register("type", "old::Actor")));
            StableIdManifest.Update(path, false, manifest =>
            {
                manifest.Retire("type", "newscope::Actor");
                return 0;
            });
            current = StableIdManifest.Load(path);
            Verify.That(current.Entries.Where(entry => entry.Id == ids[1] || entry.Id == ids[2]).All(entry => entry.Retired),
                "type retirement did not preserve retired and active field histories");
            Verify.That(current.Entries.Single(entry => entry.Id == ids[1]).Retired,
                "retired field identity was not reserved");
        });

        suite.Add("StableIds.OversizedUpdateNeverPublishesUnreadableManifest", () =>
        {
            using TemporaryDirectory temporary = new();
            string path = temporary.FilePath("identity.json");
            const int maximumBytes = 16 * 1024 * 1024;
            var text = new System.Text.StringBuilder("{\"schema\":1,\"entries\":[{\"kind\":\"type\",\"name\":\"Current\",\"id\":\"11111111111111111111111111111111\",\"aliases\":[");
            int index = 0;
            while (text.Length < maximumBytes - 65536)
            {
                if (index != 0)
                {
                    text.Append(',');
                }

                text.Append('"').Append('A').Append(index++).Append(new string('x', 1000)).Append('"');
            }

            text.Append("],\"retired\":false}]}");
            string before = text.ToString();
            Verify.That(before.Length < maximumBytes, "fixture must be readable before update");
            File.WriteAllText(path, before);
            Verify.Equal("11111111111111111111111111111111", StableIdManifest.Load(path).Resolve("type", "Current"), "large input fixture");
            Verify.Throws<InvalidDataException>(() => StableIdManifest.Update(path, false, manifest =>
            {
                manifest.Rename("type", "Current", "Next");
                return 0;
            }));
            Verify.Equal(before, File.ReadAllText(path), "oversized update replaced valid manifest");
        });
    }
}
