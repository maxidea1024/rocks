#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <Base/Compiler.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <Object/ModuleRegistration.h>
#include <ObjectSerialization/ClassDefaults.h>
#include <TestSupport/Test.h>

// 클래스 기본값 지문 golden이다(Docs/Design/RuntimeClassMetadata.ko.md 'A5c 지문 연결' 대조 1):
// 시험 fixture 모듈(ReflectionTestFixtures)의 구체 클래스마다 `<TypeId> present <editor 지문>
// <cooked 지문>`·`<TypeId> without <이유>`·`<TypeId> failed <이유>` 줄을 만들어 아래 표와 비교한다.
// 같은 표를 Windows·macOS·Debug·Release 체인이 함께 보므로 플랫폼·구성 사이 지문 차이(생성자
// 값·codec wire·지문 공식)가 여기서 드러난다. 에디터 데이터가 없는 빌드(FoundationModular —
// ROCKS_WITH_EDITOR_DATA=0)는 EditorOnly 필드가 없어 editor 지문이 다르므로 그 칸을 `-`로 두고
// 비교한다. cooked 지문은 같아야 한다(쿠커와 런타임이 같은 기본값을 본다). fixture 타입·생성자·지문
// 공식을 바꾸면 표를 새로 만든다(시험 실패 메시지의 실제 줄을 그대로 옮긴다).
using namespace rocks;
namespace class_defaults_golden_tests_file {

namespace {

// 기대 줄이다(TypeId 순 — 2026-11-25 첫 빌드 출력으로 채웠다).
constexpr const char* GoldenLines[] = {
    "030270f0-1ebf-46a8-8fe9-b961b318b41b present "
    "25671cbb9ffb87b1ccb584f392a69420b795298250939fa3e4f27b931509c795 "
    "b29dc2e0b8b9e164b3ad1de54427d434151cd70b6e729c29e9c86671f29a7562",
    "031dc88c-68e2-400c-8e0d-3ec8e6271063 present "
    "9ee92d1670e6552413413949064b49c9bbe16de981bad2463c0bb4e1ee19d0cd "
    "ac767361285225f038d2d8c041d3192d4786f8dd45eb1ba27f7100deff944ae0",
    "035bcd17-08d9-4e50-9471-0441c4751d3e present "
    "76090cdeeef19698e8d81dc4bb4cb8d99cf80a99a3d154239e02ac50f54455bf "
    "76ac233f1c8673538da88fcdb8da525de212fd7d2365e44f6aff9cfd84c29a3f",
    "04ebe20e-3113-4787-9536-2049910b38e7 present "
    "72e2d14f4c752773fc1d2c17b7bc1e8b4a1f6b79b833b0c840d6093e8af8f4c3 "
    "bf15c4493039681e7463e1ac655c41838f917174705bfab13c5ede63303ee7d1",
    "06706831-22b3-42f2-8186-50242bb3caa8 present "
    "88b608ff231eb2cd7ba764bf4e3b4e7905512389987a478da034176de21373bf "
    "1c3cc4f9c9c4298542da9314fe0e93de239ace191afafbb8d213e9ba60c5208e",
    "09aca3b1-4d94-4c01-8cf9-7e5657f77e06 present "
    "66aff3ddc903a5bac02800c6a7c060ae5dcf124859a2fd93a3e3a7c215b226ff "
    "5fbe254270b40497ffbf70cfb59a56678569a4b2218d9f8d37af1b4c5be61009",
    "0b70a169-16b6-4a7d-a162-9a1029bb28c4 present "
    "6a0d71e12fc2d69250c0d0d11f325f804c2ad4d1ce703a0ab3820656d02222fe "
    "bd9da5675cf07085c5a6a4c04d2825fb00afaf13bfcd1ac466d06e65c797d4e1",
    "0c6e22fa-b751-4451-b5d5-e44037d39b00 present "
    "1c967e28e41a8d6f8adce0125f78335667bec878788a14ceab3e80bd8fe36744 "
    "73bd5f26118c1b468aff865383fdf73ed3121f0aa65290a13a5f315618d1783e",
    "132e0676-5e90-44d2-9f05-8747614c356f present "
    "c11fc82873ba76dfc5ed411f5e3bd3d817e5bf7cdec508663a5268cd675519c0 "
    "16cefe05a82b06e8601ebf0ae4ca3e0c7ac1e82e198d9b7d32171b76932580d9",
    "1570b1af-02b8-444d-8039-83d15d74ca6e present "
    "b2949f249cdc0da8b8e807f0972fe208f51a1bbe1077936bf8ae887989c2fd20 "
    "c6fc75d73bf3d190b20b7ce75f6098247340e11931da5b5c454894fd9661f87c",
    "1aedb687-4a02-454b-bcfe-48217df89585 present "
    "2da106b28ed878253751a4b371b27b4ecfdb43ae46a8763d0805af6bfb12f343 "
    "83a9d2bb2ebe09d83e85a8bfe4d9ee2f05c6b38925a7b107465ec09c03fb01ae",
    "1d9cdbff-89e7-4feb-b842-1900a26120ad present "
    "1873a0982b04d25bf73a1668219a1a43a6581dc315d7651d9f9381568acc38c1 "
    "421aba3da253e75b279ac48d27ba759b59d835616e640c26e225e1242ab43c2f",
    "21cbfbf8-2bd1-4516-a3a7-5670e3f66ebb failed EncodeFailed",
    "21cfc71f-30ef-4d88-bf27-fe507f6f0b54 present "
    "5b2ec630d4f189a3f48b263f147188a066cc15032c98d3736e2d615fd34884b4 "
    "4451a13bd1794863b00ab280c2114aa42ad97843d6089a4f5d152798a1513734",
    "2a57318a-d0a6-4f0d-a745-8e7b1545328a present "
    "7b5ed23cede675d3489d820d76e8f86369b1aed167b8f85059e023a8b5782b24 "
    "49ca6ac394b360343ec5eeb2993dc9b981b61ea916b28517c41210c708cd70a3",
    "36030e17-bde7-4902-bf08-e0222023b9da present "
    "c8af029084b16cfd67822d23d5d43ef59b7c3136a52d64b49b29532a80ae190c "
    "0d21f2911475588b941ddd900197429efdd9eabf576a8a9a8795965c1675e0ba",
    "3a5824cb-f8f4-4956-9529-a895433e27c1 present "
    "35d6ac0686859e21a5867a053aeef2c29dd92518c1ed198f2efb5f5cecb22ab2 "
    "bcd260f9dd65e48986e4b33e7613172b81cb95837eed8b353c3b72e8b2923010",
    "3c20eb80-8945-4c9b-a02a-c0054b9b61c8 present "
    "d3a0f1df1dbd7b17c1bf3fb0cf3c11717131d66b8a755db6c575b3aa6fa5437d "
    "e5ea6e7b1469eda9113a6f58ab09e870176e8c2286738291af0d7435b8278d99",
    "3e857728-824c-4dcb-9755-fb9a9370fa16 present "
    "10132ba0c50e3bbd59ea4f83aee89201118dd85ad6a36e5723c2a0c78ad47821 "
    "137e47caf24ab671dbd222706560b706053e7a2e1432d4e6a2e5958f12c0b1cf",
    "41b30a39-560a-4477-a41e-b7ad6347332b present "
    "745d2395e7260ca157c20c8717e649ed5eae4851afffcaf0bcdeb3737d28631b "
    "0abb43225823084e89047396adc24814de196e18e8173e092ea130e81cb7e451",
    "4eaefb00-4408-464f-93e2-681eca617499 present "
    "ffad7cf20cfd9b83a3e88b1cbba04662482d256dad2763182a565f7a2d81fb72 "
    "a4d59ed2c09cfbab0ec6133a555938d8b1b67462f7500c977684024b0530171f",
    "4f044773-20eb-4280-a73b-a9445b927ace present "
    "75ea3ebf81db211bd05a11eafcb8cb7af93f4e609ddcc234c0f8943922103dc9 "
    "f9f9e3d5e811dd506185b29922ea3af4ec56faf1867b4de7cfbe3ce16a316a25",
    "5426736c-ac7f-4886-9d13-5f5e315d3b02 present "
    "45885504dcefedcb29c677039a4c9130604f67d03168d0a5e1188989ba2e75f0 "
    "6d705f1edded213ada6862e5b66fdb39d098b1c4bbcc4f3f49807a5fd87d7d96",
    "5498a1ba-3145-4b3f-927c-a772124eae7d present "
    "fea05a180cc353348abcf997a3de4c6d33d4ac73c85961ba61759422b215ab5a "
    "6e00ac55dd117974423ddfcb65e29571e33f6707e91d7c726acf71a029d99092",
    "5b99793c-eef9-47f5-a148-61cda661a2b0 present "
    "a19a1adcafb815dbbdfa8cc349ea0ff405a9bcfd92d44391f3c8cdda9ff1169f "
    "5db6de3fa1054a9f624542a003870e7de1186071b6d61bacf0f30000adcee2a1",
    "5cdb0571-c442-4422-a43d-90f12f64acd0 present "
    "647f4f7693610f246d8242bf4371b72530ff102f7262ae83f27551938f1f2718 "
    "6fbe3ee023a15794c86a93ac34c1e4d27cdebd79f786c41df24f18a3923ea4a1",
    "61e67d06-010b-43ec-a31e-06f00203a284 present "
    "e46e05e682d65717bed6d74212aa4b11c2c6138cb99dbb6aa424d0ada58451fb "
    "32fdf65231b946d8f3fcec8eb4579a025433e0223cfc76b129460bf76b41293b",
    "6220af7f-de0f-48ee-9383-a9dfc79c240c present "
    "049670bcbbc1f6ea23e79b4a377e1864725cf1bd9cae734145e0a6be929ddea3 "
    "839de34abc43e189dc68909d7ea1024542659fbc797aedb63b53e70197005334",
    "631ab19c-da11-4f21-8d17-2484c6614222 present "
    "da17939a33a73d2442be7a3a42f76446a87b0c73735528d174496d7e0d69836d "
    "2c6e39c11b8cdb5efbb5ac783a0d26500f5e18f72f4f332b8d2278e0a11164c8",
    "6c47768d-9c7e-45c8-a839-dc5f425ce08d present "
    "99ec7e1e51d3e7ae97c10a9935b7682ec4893e8faefaf86d841dc77371c634a7 "
    "c364eb009d5c0b568585ff3f4265b5edcecada36c854c3ceb3eef2962d710d4f",
    "6cafbaba-4f12-42f2-8406-76209bbf285e present "
    "3df6af2ef6a54f2fdda89e88c486b4543322af8fc2970649df0696817590f29d "
    "c7b4c0b79c8ef0a907553865de8d5cc5bc59423d77d0cfbc87160356d67e4b95",
    "6f5f43f4-ba17-4f7d-80f7-0ccd630f723b failed NonNullReference",
    "7083e353-8635-409b-b8b5-49b484dccf58 present "
    "e49b54e4fdb08f234f8e2d74c473de90a4555ce989e797b7e3ac12c051b360ed "
    "7ea7d9d68fcb4d88687165dc6960fe31a0c8192ea8ac5bc6374aebb983dcadbb",
    "74f9bf88-20ee-405a-8e20-e77a0efb7aa6 present "
    "a6d40523f6025d99571406223c0b5872abebc99572bf714db364a8a3bdb8670b "
    "bbf1d38264bf5d6c1f4855d387ba9e105699f878c26426b4de1b746e5ef82a9a",
    "75d1044f-467a-408b-98b5-285618277117 present "
    "8850b5dda7ea4a5604b3673fa31e4744abbd104896758795863496d4c4d3007f "
    "f081e2b5c9e9e63944ebfe6c4fa43c7bc0b24b2e6215fb010e4e91d7280eb1cc",
    "790bd7d9-6cdb-4661-823a-05970a419dfd present "
    "a73de56a1c5f2a8cad681e0473c72fff918784b053848e9aa3f6d9a0eff41a01 "
    "6a8490080077d2661498e256f8ed9c6d74b95dfd2cd95af4251363447f028dd1",
    "7ab1fdd6-2738-464b-ad6d-b900ba915a1d present "
    "0a17d0dbd50f7ebb52f6295a0026cef96b9b89059ff9a8ca3421aad24ae038bf "
    "ded48cff294e0647f736ae31abeed8bf622397660797786c043509054e58e5d1",
    "7c2323a8-6560-4b78-a3d9-871987d14b23 present "
    "5fb7ee5652c85d7a736bb4fa30d4648f4cfba69cb6c98ca9fcb9785ee8e04701 "
    "c6bb0f1a7f468422c95cb7ec7eb9fc3d92443ca4562d6359a2dc6d81670c9c90",
    "807393f2-2ae4-46b9-81b6-0a28f33bf7af present "
    "2b917716f2eb6368d4bb7c35b47b1cf508bd4896890e9ba2c5c9f2b06bf0dd65 "
    "a31cec3b7d8e04d30ae2aff00ebdf9a400de1642e1134accc642e5090f8b172a",
    "82ac040a-ba29-443b-be3a-87e318866bbc present "
    "eb8e70b0222b6d25d942d289b4548df549a77f745a3b5d61a03fb520573d146e "
    "66015619061e180d9eb71781bb922132accd6aa0bf3cae6a5634aef3888abb57",
    "832b22f7-6464-45fe-8602-71cf5b4a367d present "
    "4b5a1885f6cd69c47bb0fbe20114380b03a51b226e86c28d833c3e988724575f "
    "abe4d572a3ef132ef18a6f181b9d76e4dfabc5c09d68f1950af1e513cc5796e8",
    "84783dc3-c28c-4b2e-a71b-61ed536d8ead present "
    "6b32c9f991dd24ad2add165e99a5132150972676f99e5add9ef79348cb40b78e "
    "8170d8ee408983b4e26c9d0630b56b2f4fab381f587aa9741bdfa9e4556ba8e3",
    "8b12e84f-72fd-456a-b92a-ecdf51a66685 present "
    "06df599cbed89a77e9ea47220387139d9048d028e668a011033e08b80421335e "
    "c46b487fea3f74e678b19d514d3b485f8994861a1b1eacf0d3305b1474b77523",
    "8c79a226-f2a4-4edb-9e82-10325aa9b47d present "
    "b65e56bbbc8b429d3d6bf5a13b7a2dd678d37c03d79785f0960858a0ec2e5540 "
    "a3c8bc9ab9b2bd0c2abbf3439af2a81f3e5da8891ee008f3875ba1d8861cc9b1",
    "8f4b03e3-eafe-4c19-911a-1f68c285359c present "
    "57cf5bac56e36a4eab5c43549e22c357f2e7f7f2ddf92f176a912b87f26a789d "
    "adfad72afeac6066f40950328255eaf7ba9cd741c401094396d2d69a3a1f3291",
    "8fe9a370-f66a-41af-b79a-7f43450fcd13 present "
    "3dadd7c59f1696cd4a3bcea0caa6cd24bc4d25875711b27cb4e3c83ca50766b4 "
    "8d7d081085dc475335fdbee903eb717bb115c6b0ccdd69b3bdf07a513bae9589",
    "92302da4-449f-4981-a015-fab6958fe0a4 present "
    "7b7d3125175982b7c2674d30a4ada32ec6ff50d14ce99bc93ab89767559373c3 "
    "ae5580542be114c4d70e7def98e05be44054556b40c483e0ffe620d3b25d1245",
    "9305e83c-707c-4928-9b86-bf8d5d4287d9 present "
    "097c4f63c7f470a46f54ccc1e280e42669fa5b624cc964084ba15f4bc9abf387 "
    "fe34620c7b37bfa5bef85f801d51cda76917fb9346c58c1d230e067855bec4e8",
    "949ff402-32ca-4249-a301-5efe5e92edbb present "
    "9c4617865563b3b1b11f29d3543b9a070197760940100c847a7c8a19808eadc0 "
    "7a5aba363064ea7b346967df01b34d4cd2d231af1327e3a79e89f49f99278b3b",
    "96433d51-c804-41fc-b18c-132619c46e5f present "
    "abf16cef882652766ab093bc5dd184cef4e7e1aed9760d8f822a17bbf12be95c "
    "f71081737ca1bc78ff80a1c8a07ec608b9623c0f57db3f64ebce330d8a572c6f",
    "991480cb-b2fc-4a17-8670-a85a070f11ac present "
    "f4382e3b548ec1766692a17533cc9592cf533a6e5f51f42b757bf1e06372d3c0 "
    "95247c9630fc33dd59e295d76678ae18734aefcfc140e8b20fd85baedad86a08",
    "9a793a1b-959f-49cd-b976-9321c5965632 present "
    "208f619fb50bafd1769aa3551f3e95cfb28e3da208569506b25b5238d7a1c391 "
    "b60c0ed0a0f7103c70ac6d690410fbb8afc157fc1151ad246a961f17f971bfa5",
    "9e377ab0-83aa-4ed3-8a64-9140a305bbe0 present "
    "d4cc4e6e59357cc03383db69af8ab56f149d19d8b875b9682094bca8ac3a0e85 "
    "81ba86ba8343f2f2003f87901878b1535f884ec33da0b91de9f5c1fd0b6b4f2c",
    "9e67ba05-9ba1-4097-8830-e4fb90f88740 present "
    "ae5916b15f302c6624d4edeb0a5605997ebb728cb28bab3262c9ba2a8865ab41 "
    "a6b59933d9b22a2e383dfc3d244ff366da4d595210fbdcabd96b17dc8c52249f",
    "9f534398-10fd-4da1-9a8b-fd7e3cf1f0df present "
    "12dd4795ad730127958664187312f6476e0d8c699c92c5b9ae9637b808146e1a "
    "2965bbeccd3edcd0883936b07eb7b20af13add684ba076bfc60ac150f7eb5a36",
    "9fa52a95-a67c-44bd-aa5e-1daf12711f75 present "
    "d6ab1d5b1338925e75e7d79bcca91b45475ea9478cebbf802259330691f7a8f9 "
    "58bdee20aa344756bcb07409d1864ee0c0bc2792b4ef95194558b0263bffa26b",
    "a13e3a93-88fb-4722-ad99-5254266ae33c present "
    "c052210dcebdfc5d97899027a4167216ac1ebc2021ecd6127f2a84e476dde1ea "
    "8bf593762e5efcdd990d6e5522cab7c523dcd20d6649e0b72bee5d424ae3ad62",
    "a54cc6d7-b518-4346-aca1-6f963dfdb32d present "
    "747f2c210e835160c95736cc0ee2d1e70446f66135354a6e155f218088bba9a3 "
    "899f474c7c2982705243cfd4ca65a7318ae5fbedcd93f6ca280d0c01254a903a",
    "a55947ab-02cd-44ff-b9d0-e30acf9b9dd6 present "
    "cf4e847e9c875fb4ff6b1f25ef03038adf27f45447b2ea0d2e02bf42adf09289 "
    "e9a025a5c3400f422aa2a0cbdb4883f40db78e1b6188e891ce000be0225e8da3",
    "aa240d44-2807-4029-8004-f8f9bf56631d present "
    "4bc97a4f421854cc251abfe39020d6da8954409d5ec4ded09532bfde9c2c7284 "
    "3537ba6a0dac2f946189bceefbb45f9c1bfd96ecdf154057610ade8331d7c523",
    "ab0b783f-1219-476c-a8d8-10ff978e5cef present "
    "df210ea374ad309a6ae30bce3b7c34d5d09122d9b3c99a3c98a87ffb541eac98 "
    "89c301559594e92f7c85cd71d4ebf16c881e338c1a31aec733a48ea7065a1830",
    "ac1a57aa-181e-4656-ba97-86977c70aabe present "
    "aeb7c37adac3186a8722399147b064eb135f7f921851de1673d66449c6c15606 "
    "c9691f0152bfaecf4092ed849def6dbf8e9f1c26862b966c542941086607abf0",
    "ad100000-0000-0000-0000-000000000001 present "
    "48d6cac71e14b85f71e0ee77facc5342f4a261a94b2bccc9e853a82abf09e940 "
    "a89c21247706ed3b38ff7382bb354938d9b52e7ec9df3838b6704d91186edd6f",
    "ad100000-0000-0000-0000-000000000002 present "
    "bded3653d17b1d097afa46d868083f12a471bfd6945642002007b2e54b2688d2 "
    "3d5a7c61f78e78ff5dab13de7dea35ad8a9341ff765ffac8961a349834727698",
    "ad100000-0000-0000-0000-000000000004 present "
    "9c3990a69a1a511dfd5a42c77bca4107393cadecfcf7ea4f470ec4960ca88c88 "
    "278db5bffb622029a50ed6042f7ccb6c9eb5f07f845068e0db7181b4dd23e5a9",
    "ad100000-0000-0000-0000-000000000006 present "
    "8dca66f6f9229a84d4169c4f61c4eccbe7e7501afe2c500b09cac1e1f8d3f643 "
    "5669ff0317badfa1046e5b83282ea482f0dbf6c1534c8b90a99e4a26d2ce5f51",
    "b2a7a554-9504-4596-a9e8-b7e037245f2e present "
    "2156c49d1a085fd400b51b639e38406cbbedcb55e9fc2110f49848eeb271a98c "
    "d92cc259b39e2ff6d3f233867afc2bbc009b10e83ded74f9ffe271c15c344f6e",
    "b31e77f6-21d3-48f2-a666-32c78778676a present "
    "ab0d2a97531d806254cf5c3fe1d3a1687025f17a9ef2affa46d05b5843ab0394 "
    "4b83c9219133c4b438fa9b2e42b3ffa43f7157c9466f170b4ec3cbe39014d08f",
    "b38ae976-5a58-4241-b314-c6c626ecf2dd failed NonNullReference",
    "b8b2f3ce-679c-4cdc-8c93-33796caca19e present "
    "8b2189aed2cdeea5f0f1ba769d763c4a0328932a574258697e194db6551ff48d "
    "821a684d9aa5f190b16fab9a65cdba3b802d63fbe112ee477e1680157ad9eba7",
    "ba58fb1b-9c4b-4499-9d67-8afc1705f99e present "
    "a296b129d31aa85133570d706423842f969c5e85c0fd2ec5e8be009a351bf85e "
    "6729a655955ddf7f05d524da3617a38c7bccd2649ec5b86bee926a6d6a48b8c9",
    "ba6bd701-6b0a-4430-99a0-d67bebbbfb72 present "
    "b598103de97cf7269659d1bf08a2ba0f2166812dc94daec160ff047ce7fbd291 "
    "3890481e0d1c41d04da51501a67a3bb347817007d51691d2754db651d4cc95be",
    "be2f1ca4-81b4-4af2-9274-58fc1bd8e94e present "
    "0a8a3d50c78bef9a596648c97adbff43fff177f560253931bdbab9551cb36160 "
    "6cb60edea2de918b409ead8071bba9a22d04b44c076acf58cd22c1ee40735e38",
    "c0100000-0000-0000-0000-000000000001 present "
    "54f0231173ec72dd3a8a7acf2f3a10f7cfd9bab60ba132763f913b946c26481d "
    "ba0c0d916a158fcc610899211671e5bc1c0bfd657bbe818e55267bd062e0fc98",
    "c0ff35fe-b938-4482-8c09-6193be200aaf present "
    "58395b5806877341af2eb13b20e43753bdce05d90598e4ce56dac8955419ef1f "
    "3b12c97bc62d59c3d9d283b2a39aa3f36b816299736d6a883f52a5660a586ebc",
    "c11ad829-832a-43af-a5bd-2296b90c621d present "
    "a5af1a2c759fac2e57f4cae13e66091cc43e67d30f6fed0f67dc5d7acc52603b "
    "29ff9b27755f186fe15f9b677e648468ce0b5e37425cf033ca92ad1cfe99edf4",
    "c1f97d86-e856-4bf4-b7cd-38820891a518 failed NonNullReference",
    "c2ddddd7-99e3-4373-97ec-faf0bef7f698 present "
    "bfa8d116ebd87d6b96e369cd0e81ca25072b4c7c375ad7f3a0fd63373664a4a4 "
    "3920cdf47277ecb67aa1d7c2f24601537f7ac2fc6eae0aa1e14b20ac0351967f",
    "c6ff6992-9d92-4ef8-8b0e-92b8bb30f9e7 present "
    "7ef4ed270824554c73863940d4280b155437e80b4b5e7567f1bf89b549f9f400 "
    "3204ace4fbf9e6ea43fdb1c518dcefd9ff2a2f529d14be5ed6c71fba328a7433",
    "d4f82027-e0d0-4ba5-ba78-ec48555a3b89 present "
    "d7fbcba6f25573f92beb56b8afb95bc3c0dd668ee1bf3d6b3a0c57202a6c4371 "
    "05712e23c7f51e65ec3e636dce873aa6a281ab6eed815dd4d59547d3a0df7e79",
    "d7364e91-8442-46e3-bc2c-5fbc302147b7 present "
    "5085c2193c3ad25163404bfe6fd7fac6d4164995f9540ca952c5270f71e315e4 "
    "4678b492e8f803740956d2c0b66286da7683cb92fe70f05ec81aaf3b9588d638",
    "d778fdaa-4d04-47b4-ae1b-b05defec5518 present "
    "5c5c13781c18c441be8be7cc2a2bd383b61f04a1a18e58295e92ca20a2d27416 "
    "651ad4e26addcef581af3615e304a34f935870b50e6045ac065be8f347f34f2f",
    "d80c1331-4cc1-4445-bdf3-5acf709f49d2 present "
    "9a602f2bd0db385e5b9813839373e70e768ed860ae09c9e90dbf09ef73857864 "
    "28767019c5747894c49b2331f0d1f98960f5ee2cbc31bab841b09ee193424c57",
    "dc6791d9-efa2-4bdb-b9a3-5f759d6e9377 present "
    "4c11c6a925a041a00a50d7c2e3d39582c9c2c633a28007e6ec3e8e97700eb048 "
    "8a2cdbd027442dd41b3728de1ac7e48e0d1354276e6d245fb0f9a6aff1bbde06",
    "dc90b8d3-bf1a-43db-9a83-f75ff3747a69 failed NonDeterministic",
    "def46cc9-deab-4697-9bde-f739425aac9c present "
    "470919ee0c2d0477c5e3bd91f54104da4953291841d0eb5d7d416db66b535327 "
    "6d1e59694ea277dadfa04beca2b7cc9d0e4e83405384def00ff222ece79bd34c",
    "e352fe29-615f-4bb2-b476-bd12f235475e present "
    "87d1fa103950cfe4bf7d0ccca71a179978245eb287082a16c9daa1922e0873f8 "
    "07823e77db2f738d732619a2f2b4bf5bc2b40aff28d1476f14b821f950840c8c",
    "e42cd2ab-3ac9-4426-a908-5883a492a601 present "
    "a00040375c91c37fc1cbf8bc7ae162dce594ecbbe677424ced1308396580c605 "
    "f26759dbf51133b815af555e2e49723060357e1b9c416848d71a135598d43cc7",
    "e695fedc-040e-45a0-8cbf-8f68a53cac60 present "
    "0d3daccc06781f9956b4e1ead84ddb2993cccc7176fb213cf91b95c8dc2040b7 "
    "aee5cbcfa8b0276bdcb97f0353c455cf5e544448a6247e9c07f112585b7966ae",
    "e7932f1d-4d8d-46ec-bd51-587753b4367e present "
    "0f0246da26bd7861e9a88402eadd914873a605568262c4bee70fa4cb1e3cf26f "
    "3f01f9c381bc2eff51d2bfd81c1dede48e654bb10411c418b6eff7072cd87065",
    "eaebb41c-f48d-42d8-8cea-62952cf7b8bb present "
    "e7f16b0f5cd569d483cdd36bc00995f785769be1c9a0171fca4b6c2469da1f40 "
    "b7eeb9390a6a0dee2a344a89a68a63b6760c411f902e9fbaea3a73ad678290f3",
    "ee3a89e6-f29d-47bd-99d9-3ef9fa2f4982 present "
    "8607f6be1a70638e028255866a749c6e382b3939fb6031bbe7eee23ad135c361 "
    "6f8639b89267e973ac61a67d22d4e1f4598b8017c07c5ffbbd5a6d508c69adce",
    "f51103ef-6007-4fa4-9ebc-af462cc6d8a3 present "
    "78bd511a987db639ad791cbc7b022e45d4f65c0e1f7dc979e4ef6662dd4e2c2d "
    "279394fae3cda937f4bfa7fb1516d2020849c828da43b18e4785ec288e28efec",
    "f7419fd9-8b7e-4c3b-a6d6-0cb3064b97bf present "
    "c9944d31e0ab11c737839cf7907f8951044003115ac8efa8ed4701b947749be4 "
    "48bfc40f117dfe9a483107b2d25e564cc58b6b490640810a4933360480b1458a",
    "f84efdb1-9e97-4c0a-a334-ba161c400144 present "
    "ff36dd9e585ccd460c1a8fa0c80c204972af0d982da45522de02f1d5606a87dd "
    "f087a57b3ea5474f32981d6a1bb7ad6029843674b7cd431e39c19a47b62e0477",
    "fa56cb13-8bff-42e3-a21c-d940aa268446 present "
    "4b3e92274badaae9f587be42bb835b7df95b15d3473304e3de6fe6abbc312645 "
    "3a2f68376944ad37bfe8ff499b44190490abe5d33e6c0e440601d724f6ab6715",
    "fc1bf068-0143-4b0b-96b3-bba657d193a3 present "
    "e715568df46a7386d46bf2a2f251543a155062be2f0d01ad18b38effd0c56f2a "
    "bcf20fe91ee1638a1c2ded893e7b147997d394d835023c36686a8c57c0533c4f",
    "fc97575b-785a-4949-a70a-5372232ea094 present "
    "a2e76142165c31f6a73f3f825d5bcdc1678ae437943416b96aab7ae1270b3c1b "
    "2207756baaf7fb71694d1b52f3eb35d564cb1d90576aad55829200ebadfa6231",
};

struct Collected {
    explicit Collected(Allocator& allocator) noexcept : text(allocator) {
    }

    StringBuilder<4096> text;
};

Result<void> AppendLine(void* user, TypeId type, const ClassDefaults* defaults,
                        const ClassDefaultsDiagnostic& diagnostic) noexcept {
    auto& out = *static_cast<Collected*>(user);
    if (defaults != nullptr) {
        char editor[Sha256Digest::HexBytes + 1] = "-";
        char cooked[Sha256Digest::HexBytes + 1];
#if ROCKS_WITH_EDITOR_DATA
        defaults->Fingerprint(ClassDefaultsConfiguration::Editor).ToHex(editor);
#endif
        defaults->Fingerprint(ClassDefaultsConfiguration::Cooked).ToHex(cooked);
        return out.text.TryAppendFormat("{} present {} {}\n", type, StringView(editor),
                                        StringView(cooked));
    }

    return out.text.TryAppendFormat(
        "{} {} {}\n", type,
        StringView(IsClassWithoutDefaults(diagnostic.reason) ? "without" : "failed"),
        StringView(ClassDefaultsReasonName(diagnostic.reason)));
}

// 실제 줄을 하나씩 실패 메시지로 남긴다(표를 새로 만들 때 그대로 옮긴다).
void ReportActualLines(StringView text) noexcept {
    constexpr usize MaxLineBytes = 256;
    char line[MaxLineBytes];
    usize length = 0;
    for (usize index = 0; index < text.ByteCount(); ++index) {
        const char c = text.Data()[index];
        if (c == '\n' || length + 1 == MaxLineBytes) {
            line[length] = 0;
            test::RecordFailure(line, __FILE__, __LINE__);
            length = 0;
            if (c == '\n') {
                continue;
            }
        }

        line[length++] = c;
    }
}

} // namespace

TEST(ClassDefaults_FingerprintGolden) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        Collected collected(allocator);
        REQUIRE(ForEachClassDefaults(types, allocator, &AppendLine, &collected));
        // 표가 비었거나 다르면 실제 줄을 실패 메시지로 남긴다(표를 새로 만들 때 옮긴다).
        const bool filled = GoldenLines[0] != nullptr;
        StringBuilder<4096> expected(allocator);
        for (usize index = 0; filled && index < sizeof(GoldenLines) / sizeof(GoldenLines[0]);
             ++index) {
            const StringView line(GoldenLines[index]);
#if ROCKS_WITH_EDITOR_DATA
            REQUIRE(expected.TryAppend(line));
#else
            // `<TypeId> present <editor> <cooked>`의 editor 칸을 `-`로 바꾼다.
            constexpr StringView Present = " present ";
            const usize at = line.Find(Present, StringComparison::CaseSensitive);
            if (at == StringView::NotFound) {
                REQUIRE(expected.TryAppend(line));
            } else {
                const usize editorAt = at + Present.ByteCount();
                const usize cookedAt = editorAt + Sha256Digest::HexBytes;
                REQUIRE(cookedAt <= line.ByteCount());
                REQUIRE(expected.TryAppend(StringView(line.Data(), editorAt)));
                REQUIRE(expected.TryAppend("-"));
                REQUIRE(expected.TryAppend(
                    StringView(line.Data() + cookedAt, line.ByteCount() - cookedAt)));
            }
#endif
            REQUIRE(expected.TryAppend("\n"));
        }

        const bool same = filled && collected.text.View() == expected.View();
        CHECK(same);
        if (!same) {
            ReportActualLines(collected.text.View());
        }

        CHECK(registration.Unregister());
    }

    CHECK(types.Shutdown());
}

} // namespace class_defaults_golden_tests_file
