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

// 기대 줄이다(TypeId 순 — 2026-11-25 첫 빌드 출력으로 채웠고, 2026-11-28 graph 형식 3으로 지문을
// 다시 고정했다).
constexpr const char* GoldenLines[] = {
    "030270f0-1ebf-46a8-8fe9-b961b318b41b present "
    "0ad998d020a27c551183a0bfeab0c7f72ddd58b8fe61b6abd1f40d5efc6d9dcd "
    "81d17adfd80d5e53dfe5e2f41599fd209212688b59879d5e84e471f36e9eac9d",
    "031dc88c-68e2-400c-8e0d-3ec8e6271063 present "
    "9ba8550bff7e05f8e64bc7064ac21f41479b693bbebe609705839dd62a05e94a "
    "84f92482499638e87ab9913e6450c1479049e39d3231348cd8c9323d5648ea87",
    "035bcd17-08d9-4e50-9471-0441c4751d3e present "
    "4fe1c3a3b1e59f3b69922a11c6349eee4ff3d95fb1e6a2e9c9107f063f070318 "
    "2536d54a72583690ddf0a324154dfcaa4c629ccffd9d17bdfc516e5cd2d4b111",
    "04ebe20e-3113-4787-9536-2049910b38e7 present "
    "75f1e4a2ed547ae478a14a5b3200a46072a55c90c9848d9525bff11a85d06602 "
    "02cc0ce73ecc8c242a6ea34688ff9e574300a935492f59c061bb4234560c869a",
    "06706831-22b3-42f2-8186-50242bb3caa8 present "
    "f1a3a16a20e45a4fae54d1b350923a2b2788322718d7305f9f97105f6eee5d2e "
    "a844b7f607f540946c2bca6d323209dc0fca1c3b1153fed97978b7beb89099d7",
    "09aca3b1-4d94-4c01-8cf9-7e5657f77e06 present "
    "7abead2862f93d1fee54bd4902f4a04fc463a5d7aea1f94cf7a7c1d2133ac0cc "
    "b44b0d9a64b235c754d4a15e2b4766c508228f730540ba00c036932036cfdcf2",
    "0b70a169-16b6-4a7d-a162-9a1029bb28c4 present "
    "991e35c30f3fda0c9a15b73b96be26ebb69f614469293919b30f1db2e998830e "
    "17262143436a55f91fd8529c8b4fc1f5225319e26ac28cbaa03c03508b9a1e18",
    "0c6e22fa-b751-4451-b5d5-e44037d39b00 present "
    "ee14bc1329dace30182f7c60cad3ad4ab0913b1a8c61b770ce276e0911a79809 "
    "adda9b3c5910d960a74359a08992a8c7771125dca734950e4c1fd9b9ec1ed048",
    "132e0676-5e90-44d2-9f05-8747614c356f present "
    "c9939a76dbd8565d280cbdc4a95e962b3ae605fcd3e7b7534238cfe056a4ebff "
    "63eb81f02b462314eea8a989b87f2da6c7f12b1ae0cae5c5d7c1d43c58b593c7",
    "1570b1af-02b8-444d-8039-83d15d74ca6e present "
    "5b51d20609f9ca98dee256098513b680a8063c99dca944997a9cce0574cc9f21 "
    "3266f0e7f1dc598c41aab4d09039f5b12fc170f136f25c574a39e8c745fead99",
    "1aedb687-4a02-454b-bcfe-48217df89585 present "
    "80d568f9d3cfedcd333b83e0e1f7b953bf9da81ce51cfb3815a3f4823918dc5d "
    "98a999aaee3ad9bc9b79eee976865b1d93266456094ae9e275d0b67ff984ea3f",
    "1d9cdbff-89e7-4feb-b842-1900a26120ad present "
    "fe0bedd7504b66708307669aa090363143e886948a5ffce262025c6f52b05eef "
    "58194a8642e81510e36ca506eddbd7554653170653fe728b22807ffe7550508c",
    "21cbfbf8-2bd1-4516-a3a7-5670e3f66ebb failed EncodeFailed",
    "21cfc71f-30ef-4d88-bf27-fe507f6f0b54 present "
    "7b0b342a2f44a6072791219e7f5e0941120777df3223028fa943a6be3c586c7e "
    "0b74e2aa7ccf03034d6d90995f7714ffe5cf4bd2b385f83f9544496bf5b4fc5d",
    "2a57318a-d0a6-4f0d-a745-8e7b1545328a present "
    "3cf454957a369eef403959362049f529d35aaf36a39f82f08fa0c3b06f344b3f "
    "d1b2001c0c3cf9fd8cd89c272516809f7921ef0cf7ee918c30a74c74a0ca3a00",
    "36030e17-bde7-4902-bf08-e0222023b9da present "
    "b02e4398145387bc477a57cc4cb6ff21ee3aac0a920e6fea2f32108c524fa0d6 "
    "99541c829494d7452644a2aa5fc0f30a851c149faeccbbf3180a40423e06779a",
    "3a5824cb-f8f4-4956-9529-a895433e27c1 present "
    "38aa6281e9663e15fa84f2c157f4e4a88b1c190b62aeccbcc98975dc43627ea9 "
    "c9f5a43874649d99788c566a16c5d438d25c9e00529aac243e92c5b4df26fffd",
    "3c20eb80-8945-4c9b-a02a-c0054b9b61c8 present "
    "b83d5e1a4ba6e416723eee1fe8d8faef395c9bda599838d07971de249bce6b6c "
    "4bed9028c1f042bba6221f46354ce3404bf2fb8b6d162816f7161b78ad12293a",
    "3e857728-824c-4dcb-9755-fb9a9370fa16 present "
    "29f4b14950323227876f8e171913c9d50ee145f94ae633b3dabce5b2e04a2886 "
    "610e74ddb472f344b9358cadc0fe5f0c213b0690dba30a92dbbfc93282ccb978",
    "41b30a39-560a-4477-a41e-b7ad6347332b present "
    "16a975534f35c575954978c5bd9320cbbde743cb39de393be279fd09b077af25 "
    "c20eaa34909f43123f9d34ed0a7fb387190fe8110f81b1649fb072b2d58628ba",
    "4eaefb00-4408-464f-93e2-681eca617499 present "
    "a8c4e5678ede8cddbb5d70e01ac3ea4d71393014517dff7b262b7004782d5c15 "
    "96000868d3d96e37eb74fce1abae81d51092fadc39888ccd9e0dc08cd03113d6",
    "4f044773-20eb-4280-a73b-a9445b927ace present "
    "342bdc53fdec4b0c0875b51369ac062e460ab7bc8fbe8f1a2c678fc801514764 "
    "96866108304f7facc4c546874cd8a685a20910d7ea3f8643c963669ec25d2691",
    "5426736c-ac7f-4886-9d13-5f5e315d3b02 present "
    "c6e9b0f60c63ea0b016eb8ccd7008cc1ca27e71fbbb644a061d1b1409686d04c "
    "660c3aec7bfb04bb61f09616cdb83fb38b1cb55ab632bce661fc2e7cdab4b00d",
    "5498a1ba-3145-4b3f-927c-a772124eae7d present "
    "71dd3acf1f03b2489fcb7beaa975d043f074f3215020d48a1126eb3b25329bac "
    "fb4213d9e21b082da523122291fc5b39f807006d90156009198bd47192d2fc11",
    "5b99793c-eef9-47f5-a148-61cda661a2b0 present "
    "50dd6627eac0c8edb2d958034857f7ec9391c6dda3f0adebace8fcfb1d857cf8 "
    "fb0ef266dd4cc1ede7ed590dee7fa2bf2948393f4bd72e4da906037732042484",
    "5cdb0571-c442-4422-a43d-90f12f64acd0 present "
    "3287ffbff369258d1bc9a81bb79353447864eaecec7f2b113da1c465bfa1b619 "
    "2468cf0b2fb2efce056b0ce04f83dcd65a306a0663d6807c1658277b14746d3b",
    "61e67d06-010b-43ec-a31e-06f00203a284 present "
    "504a39b12cf5bf27a03631199104355e87a9469df60300208ae0c72ed2e6f737 "
    "8fd8d877ac5a75f63a84ac536040fe507123cb103635ae6b8a1c640bb68ce97a",
    "6220af7f-de0f-48ee-9383-a9dfc79c240c present "
    "5e9673abc60dd188ac710c9bfd4781c2b7d4441b0784aed936b97a98c357238f "
    "d825c708ab2fa0fbdc7b917eed15c9b5f5b315362597b79eab4d9ff3a16f05f1",
    "631ab19c-da11-4f21-8d17-2484c6614222 present "
    "2c68a35563704fa14e11d39684c831c74882d0d3186c243a5c03dd1ce6fb752f "
    "e9266faefce5206af22beec6a148b823243eaf34be8c80bac3be518edf1be00c",
    "6c47768d-9c7e-45c8-a839-dc5f425ce08d present "
    "ad69931f04fce93107b6087bd98c42cff630b483294b7af14bcae65c0c322ee0 "
    "0e4cc896109f3faa5830304eff269a360cc182970ea0bab7214b9a5da8ca5841",
    "6cafbaba-4f12-42f2-8406-76209bbf285e present "
    "8ff38b65a7aca0281cfa03fbe1e4047ce671c8cacb8762bc17a56c1455a6e484 "
    "c63a9d4fce7d29eca8c58308d7e3dee0660d033ff8083bae5b4e576cff3d518d",
    "6f5f43f4-ba17-4f7d-80f7-0ccd630f723b failed NonNullReference",
    "7083e353-8635-409b-b8b5-49b484dccf58 present "
    "6d29522ff9d3ad002761713247699651d7cd80888dd114af968baabefc7032b6 "
    "de6867c1d3a87c7a1629f3f07bd811c8a9db26590f08694df3762db1ae6e568c",
    "74f9bf88-20ee-405a-8e20-e77a0efb7aa6 present "
    "b6d28d881aa8b5ceeb8f7d7b3e3035375913bbe0da98a836e96c361e1ea76f13 "
    "ed46f5ad7e88bc15015994cc74d5cecf56a93219103cc056f689d4bf5556f39e",
    "75d1044f-467a-408b-98b5-285618277117 present "
    "c5b6ad6633a1cfa91cb2ac7a9450d42117bd22e67aa456a15cb6728b272b930e "
    "f3d5c9804ef9358c6046da001403628593f5666f8c956e7ca047c0267ae57fb6",
    "790bd7d9-6cdb-4661-823a-05970a419dfd present "
    "d66ab0646771f72f672d70dd5822056c344f1fb0deb6719f93804175d7f4cc2a "
    "3347080d87c492d5c879347c675fd5cc05159ffc8d8d2290460e1fe23c7c8069",
    "7ab1fdd6-2738-464b-ad6d-b900ba915a1d present "
    "8d4d9c4509d20296b5969947a7887f0db4bf4bb46cfb0b8316f4f4f510284c24 "
    "0cb762eaa1b1a4d8e729a83628db049139b57165b148fac706be72a8dfd0ed1a",
    "7c2323a8-6560-4b78-a3d9-871987d14b23 present "
    "d0d301d7f1eb92fd88af8f0fa15810c5d5a1f6bd94f7812b8e05b8765e7ee0d3 "
    "a9a55bacb05ce43da8884c7f090b685551211de36d3dc5766763dea40c48dcb9",
    "807393f2-2ae4-46b9-81b6-0a28f33bf7af present "
    "fc8b39e9affcbeacd9c275dce41bb6df3269d55b8a09227e63991b22e6988b43 "
    "4ed5b49bfd75146b758ece345bb6b8baf2eb26b214ca190a7a8dcfc822e8ee7a",
    "82ac040a-ba29-443b-be3a-87e318866bbc present "
    "75a0aec0507e920f63b4584b50e69c281e27cfcaf4a740a74f793153006773c6 "
    "201f6748e66bc4ab64ea268114ff075dc0adbf57ae117b918baba8f3f526cb92",
    "832b22f7-6464-45fe-8602-71cf5b4a367d present "
    "9861fcc62ed0d60fdb3f12e7a07d8d6f6ae85a4f0d091543f69ca5b91a0b65df "
    "e15106c095b11bee6a66947000ef3d9888e0e8b760d655c74293800bc73ba917",
    "84783dc3-c28c-4b2e-a71b-61ed536d8ead present "
    "cff8eea04d415ea67a323347a53aa1d4014f562ead94b79fe9d18c53bd1b99a0 "
    "101c80e564fb7b5ea898479a5771b04cc1c9ae48b452e18cfe9f18239358d88e",
    "8b12e84f-72fd-456a-b92a-ecdf51a66685 present "
    "f443fe0d0b0362cf333adcb946c6e7d4f2455d78a31fc15ff958ade8a1af0128 "
    "cd174235c54937a8b50197994bf154766c40c57f4184e520e783be70b2fbbd7d",
    "8c79a226-f2a4-4edb-9e82-10325aa9b47d present "
    "87e9fb0c68685623b2175887b9f837414289b681c4e6578126bd04ce843b57ea "
    "109cf8ea5b2da4f2c1a8e979646b9aa135c79c662cdff4eecea22a1379cd68ef",
    "8f4b03e3-eafe-4c19-911a-1f68c285359c present "
    "e5546bb31df3548815adb40a93dfb8a4409dffc2f01f58e1a957057fd13e8adc "
    "b9163c32c90009060f169f3824e76f4bfb0797e685374a6b87113a7a2978d425",
    "8fe9a370-f66a-41af-b79a-7f43450fcd13 present "
    "fc5af2537e0826a39eeaf3623a455031f783b09505b871da4144cc4a84708b5e "
    "02c8831eb4da90e6a37adcb2694476ac593606a691711b53acada698a99d5a06",
    "92302da4-449f-4981-a015-fab6958fe0a4 present "
    "86af4d554ddf6ff15690c11f8dd8b9415bb050fde22ebb4581fb1bfb8bfd710f "
    "f13115dce4f758b78477c493e3cece7585dfa116b633ce9b6fc04c62188298c6",
    "9305e83c-707c-4928-9b86-bf8d5d4287d9 present "
    "6f2bbaaef65ca8e39ab235be1c52c0975d10d64c1c60e8aa62e2d51013387fa4 "
    "f002a44d2974d6333bd39a47037030e9f0f550719977a8b6cf97c94673de64ba",
    "949ff402-32ca-4249-a301-5efe5e92edbb present "
    "390412d0d9aadfc41673d350171bdc41136f8a1f3fb7effe6213315589828116 "
    "0558e87d905210df9534988fe681ab827a8d32384ae4b8ebfd8a8f5a654b48a6",
    "96433d51-c804-41fc-b18c-132619c46e5f present "
    "f6cb4abfe3466c2f12cf526eab0fd85929cd26aa33a6958773fbb9a8b9c97477 "
    "454ea56e89b676f2d74abd5336fca7783a7d135e8d8f136bbf0e96174f9e9fc8",
    "991480cb-b2fc-4a17-8670-a85a070f11ac present "
    "ddb989db75f0a42dc0d8f551c5e4e3a94d09a1c170834e7f05713d9534864933 "
    "b352e633c01b15a3d4c34b4934d3efcfa9fac43e6ea1a6c3519ecd9b23bcc9e1",
    "9a793a1b-959f-49cd-b976-9321c5965632 present "
    "91e12edcd8e5ff16bf51ca7682ab4f69fb65107558da55a041409123f09692fa "
    "9315622ce1f839aed19ccb3cdc0fb7c37cf8984ab3e4662847a158dfa75864a8",
    "9e377ab0-83aa-4ed3-8a64-9140a305bbe0 present "
    "4e32223823061bb082b82062d960fcec34dea2aafbbcbcaec048718b092bead2 "
    "b91a7e2f7a22fa10720446b6b48a22a8f23af63fc287f4447e4a3f9e2a20c91b",
    "9e67ba05-9ba1-4097-8830-e4fb90f88740 present "
    "23f037ca2cbcdef5dcdc937c1c648f3e3bb7b210f1356eec8c0535eff8418f88 "
    "74e13069ef073858db479be03414bdcf4e0b67921b570700e3102f6eaf856cf3",
    "9f534398-10fd-4da1-9a8b-fd7e3cf1f0df present "
    "0c5d670bde9d9a9e1a99e98f27f635197f6ca2c505461bd454e08233f797443b "
    "8091713ebdab37bf87662e97499265fd967b3cd76d55bcc1658489184b691f38",
    "9fa52a95-a67c-44bd-aa5e-1daf12711f75 present "
    "f13ea6dcfd4f866d8de7ed035a252347b85289ad7a9897a5725116da29501d3d "
    "bcbf76245cc673f897d172ebd4f87e0a23bbb4aef5c6b77daaf3393ad52b96b3",
    "a13e3a93-88fb-4722-ad99-5254266ae33c present "
    "67d3a0062d3172dad2e023e7ea2a08f2aa1e109fe1dd68f98f9ddd9446535f0b "
    "9d17d6c5df3ffa906c558b6d6f77ba9bb583dad50c332b2c201fd59cdcf3dbfe",
    "a54cc6d7-b518-4346-aca1-6f963dfdb32d present "
    "8bc4f7573772213bbccbfba48b6250a9798b179f96524ca5cb093d2e5a3253eb "
    "925697f08f2005ca3a9041504e3fb41f0867655da5b71bc90d97d99b438cb110",
    "a55947ab-02cd-44ff-b9d0-e30acf9b9dd6 present "
    "7ae4bbfa9309d382e8e590ca13c28b864cbc1d6f24613a15a5ab8d7466702e32 "
    "9f632d0089449355eace750381c76c33866a4c0f494433dc6975eb5a0429dc46",
    "aa240d44-2807-4029-8004-f8f9bf56631d present "
    "6db7b7007c7d190898c84c226a5821f3ef8d7125c75e1f203f77fc9d4e38ce78 "
    "7b4c88da9cab6e9ac423b5ca9bfc32b6d67e64d7cebae13bf404f115cf8a12f0",
    "ab0b783f-1219-476c-a8d8-10ff978e5cef present "
    "4a0addfda9f928a19c549ab224bf043676aa509b939faf3820bd4a3478894996 "
    "83ede0b442363e5882ae53de6e7a4ed770d4c232d2516f0eb8920bca77da4716",
    "ac1a57aa-181e-4656-ba97-86977c70aabe present "
    "47a7ea0ea710741066a96889fe75da9daa258acb77eb096b5c90d2842f40e17d "
    "180e0eeb8a7cf7c3081332e500eadc5cddec6e10daddcf5ae1c9319e852018c7",
    "ad100000-0000-0000-0000-000000000001 present "
    "83d8e3b77d98d5e2bbce59c015467108ef346616e1e7242abdc8b02ee6e6ab36 "
    "8ba668c73f67c6eb456ec126249614bac2ae0138c5e60b06043fdc2cafbce74b",
    "ad100000-0000-0000-0000-000000000002 present "
    "27948c6ed5306e123769931661ecd41a733523196721392893f5b283d30f7102 "
    "4bf3938bdd120bf09c1b18dd81e993c2b9b1642beef4de123c5dbb30b447e94d",
    "ad100000-0000-0000-0000-000000000004 present "
    "f7837748035ed096bf6b0d700046797b6ebe45170e6daebcee119411d4c5f4f9 "
    "c1c0ce28a5162fcef968cfcdddccafa0f453d19d0fec8c7f4eeb24d22d247b2c",
    "ad100000-0000-0000-0000-000000000006 present "
    "80faf222ee54b10f1f290c68cba0e48ea0eec801d4e4cf9a0156b579c3e8bd6c "
    "54ba279fdb0e9fd45c2d4bdcb857544c91217b4136935f0c0419e9dfeb28af1d",
    "b2a7a554-9504-4596-a9e8-b7e037245f2e present "
    "8e69666d3c1b5961e1ddaf6d0ea2aa27356667034cbec71b595c7a95ed3354ff "
    "3ce743293edf24dca6179e596b5d455cbf55bd1a2a0ef286389a89e77d972398",
    "b31e77f6-21d3-48f2-a666-32c78778676a present "
    "ca344ba15757ca62061807bda31b3686441ae631649bfab3cf5d60334a24ae9c "
    "ca38eb3d15ffbd7a5daa6739f31bb1e28c6b7bd54d92d98cbf54eb537d3dd2bc",
    "b38ae976-5a58-4241-b314-c6c626ecf2dd failed NonNullReference",
    "b8b2f3ce-679c-4cdc-8c93-33796caca19e present "
    "f7a767b92f5837494db77240736c0c81e20eb942c5349231932d8663348b7d78 "
    "049b58d0242630f378d1a1c59ad705a3dccbdb04dc85273a595e2107ad966595",
    "ba58fb1b-9c4b-4499-9d67-8afc1705f99e present "
    "8d410ef09823f788f493d6f1bf0bc7077ed7fc33ac6c79f1c30122ce4ae643b4 "
    "e7704f16330d4a27cbcdc1272ee72297e3b692604839994c044ecd348196e684",
    "ba6bd701-6b0a-4430-99a0-d67bebbbfb72 present "
    "35993521aaac8d783f20b82ed4489d33d8fc76d98bc06974d3491672774f43e6 "
    "28ad35b39bb6afa98d437ec5f665e1c7956362c02d2742d1d0ba9fb0984624ca",
    "be2f1ca4-81b4-4af2-9274-58fc1bd8e94e present "
    "c15bf6f3a4c3268b63bdeec7a0a7955754982de120f0702d49b4109c41b21499 "
    "ff76fde503f905c822e27b0ebe13fbed0ed60caeae687d4e6b10debc6723e86b",
    "c0100000-0000-0000-0000-000000000001 present "
    "4fc3932940fe846f63145115fad5a264a63b8fab94b602d2b646ed7c4ac16b95 "
    "b80d491e5f566a28685c9c22558cac056200365ca5c4c297708e6330d349d390",
    "c0ff35fe-b938-4482-8c09-6193be200aaf present "
    "c5f33cc0140e2c7a88d08e9774c76efd705367f6f562448c6965101fc5568a07 "
    "2b187d301c9955cb067a2a78d05fc7896f17097b12195761fde39b7ef46b161f",
    "c11ad829-832a-43af-a5bd-2296b90c621d present "
    "f5702ae5e9d64652458582413aefdc43e791285f793a38b01b293b1487cc545d "
    "e6ee7326f8fe409b5a67f2964408c2645eaa55bb6f035733f1bf396b52f1e49b",
    "c1f97d86-e856-4bf4-b7cd-38820891a518 failed NonNullReference",
    "c2ddddd7-99e3-4373-97ec-faf0bef7f698 present "
    "722c97b6d8d7a0da01d709a570e9e9d0ceb68def63026cc467bbc6bc397a10fe "
    "fe809d93a785c0694c5ab4b6c98ad9e12e09808087620676afb14c614c686bb4",
    "c6ff6992-9d92-4ef8-8b0e-92b8bb30f9e7 present "
    "3893f62945261a5976eb28339086d9c012f773035e3cf8c2e9a1035b0d640cb4 "
    "d7fd512618be4e792ded93c1c3f79defc08aa6ba45cb16b019b1421711348175",
    "d4f82027-e0d0-4ba5-ba78-ec48555a3b89 present "
    "d7b976291d1f8dded417548710a70a0949a0b7fd95e545ec73bccf977f9e40f1 "
    "896e98dd2774280826839c23a499e61048b8d1bdac19e65bee7544dbe8217252",
    "d7364e91-8442-46e3-bc2c-5fbc302147b7 present "
    "0ad10aaf422e0ceb8f83d6ab706cd6c5ebcdfe22385e383ceb1d1908095eb6b1 "
    "177cce317c354d46a2d2877228f80eefa15958f66f43d58e2d0dd9116274b2b6",
    "d778fdaa-4d04-47b4-ae1b-b05defec5518 present "
    "61ddac9eb7dfecfaca32531ab7ca3f9fe7a91b125172bd0dd07dd46e5d1d72e5 "
    "0aa93a503e168b3138ed36e52f55537ebf9e15d0cfde48620d2fd31b0601b0cd",
    "d80c1331-4cc1-4445-bdf3-5acf709f49d2 present "
    "97914d4ece82537de22711f90d62048158d230c7c427b4ec23a3e55d8ae6cf04 "
    "43b04407db49e0055c75b28c88f084163c6b1146f1e0b9dc1355124781f95e98",
    "dc6791d9-efa2-4bdb-b9a3-5f759d6e9377 present "
    "f0969f4b9f77f2063440fc87c3a1194d1fa50fb61218bb4503c60a158cd7e744 "
    "ce919eec4b0ada75644117cf0e138979de47f6e05685ea43aaad4cc2308e34d6",
    "dc90b8d3-bf1a-43db-9a83-f75ff3747a69 failed NonDeterministic",
    "def46cc9-deab-4697-9bde-f739425aac9c present "
    "b978020941bb4d4d14ac0624ae0e76ff29c9e662d7fb9637014169af07ac5f02 "
    "66598b1ba419d10e5cd8c12261e223791a57c71dda2379ef329210ff78b29b55",
    "e352fe29-615f-4bb2-b476-bd12f235475e present "
    "d170e3e0f453641b5fdbe98d460420187456dfc685bdeb031ccaecdcca55c2a1 "
    "7cc4be9576a91e5cd0eed90b370e9fc29695344f2c7cd1ea10b3f52b374df95e",
    "e42cd2ab-3ac9-4426-a908-5883a492a601 present "
    "c715df56608c3ece6650acd9354e65970a49bbf12b467de172a640bab4377b74 "
    "067678fc19cf5efd75994c6b1ba7fa0eba80ca282d293638ff9a86640efce2ea",
    "e695fedc-040e-45a0-8cbf-8f68a53cac60 present "
    "abf268b76770e1c53a6d6f70c25aaa6724ecea1d57e779f4d9c9d13b52aadb6f "
    "9b9d094684f9e6afaa2c4588cbcb2c4832a55d37a1cd4e757b0064a8beeb62d4",
    "e7932f1d-4d8d-46ec-bd51-587753b4367e present "
    "0badf2a47f29f6ae893af795de59ef0c28571e7dc12d8466f227c4c6e66e88a5 "
    "345e3ec376e6af962b365c217a4b8c6f923e7bde00b0f0fccd30954b83084631",
    "eaebb41c-f48d-42d8-8cea-62952cf7b8bb present "
    "c9bac5b373f04293a650785799c99f927771fbc9238acb51c91ba70810fb2178 "
    "bd2d19cc4708699614af2d055bed2b1c0c7bb0369965eff9c5e8e93fd21a099a",
    "ee3a89e6-f29d-47bd-99d9-3ef9fa2f4982 present "
    "dff6243bcc09c8274b4520844753498e061ad133bd5e1cd21842c5773f5c38c6 "
    "30e27e67758d727c84bd8726b87e0e01c1afe158dca4674a766b2cbe14d6ebf0",
    "f51103ef-6007-4fa4-9ebc-af462cc6d8a3 present "
    "2e7e1831ed832ed21159e232a128c3b63716c20839ebf7c6e0be0bcd7ef6d55a "
    "328fa3b2af469422fa8d519c64516c956ae1d161e386038ae84fb1008d21f2e5",
    "f7419fd9-8b7e-4c3b-a6d6-0cb3064b97bf present "
    "cc87e782aace08a5e9d35d7c63890e2d5920c7c110e92b35850191d40c9982ce "
    "0c57746713d6126e732bf42d93a3ed06ee926c583c1c31bd49af671217a180fe",
    "f84efdb1-9e97-4c0a-a334-ba161c400144 present "
    "88d3f45771fbbf3274f905db2dcbba52044d15f5a2d870c3cc3ca72e44997180 "
    "047b2998812cd67b2ce3474cc6d76f196bc2506c0139ecfb21a115cac8a736ce",
    "fa56cb13-8bff-42e3-a21c-d940aa268446 present "
    "e23c0135432195d0b36d37cac8ae731ebc47285af1c79af30dde8bc0a361bd5a "
    "953952890d9a10a46d877ee6e2babaaa2b7e6a765acf99322300fde51bf76de9",
    "fc1bf068-0143-4b0b-96b3-bba657d193a3 present "
    "31a1f71de164df0470d41225e1a2cb3600e4d902f812a3d6b1e1e894d81b5553 "
    "6f716f4fef923561e9a59fba6968b483cc1bd52665e71cedf3fb28a05c2b8fb3",
    "fc97575b-785a-4949-a70a-5372232ea094 present "
    "6cdf9a0a015d2513c809c8ecdee806b4fb66107298a8cf3d983b1369af515bd7 "
    "74b0203554b0463de0ba0c1e0ef6ae5e4b7a9ecb0a72a970568a77c2ce0c75d8",
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
