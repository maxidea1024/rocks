#pragma once

#include <Core/Text/StringView.h>

// graph-dump golden이다(Docs/Design/NodeGraph 13절 GT10): 표본 문서(NodeGraph 시험 Sample)의
// 정규 dump 텍스트. 형식(9.4절)이 바뀌면 시험이 실제 출력을 보여 주므로 검토한 뒤 고친다.
namespace rocks::nodegraph_tools_test {

inline constexpr StringView SampleDocumentGolden = R"golden({
  "format": "rocks.graph-document",
  "formatVersion": 1,
  "domain": "Test",
  "documentFormatVersion": 1,
  "mainGraph": "00000000-0000-0000-0000-000000000001",
  "graphs": [
    {
      "id": "00000000-0000-0000-0000-000000000001",
      "role": "Main",
      "nodes": [
        {
          "id": "00000000-0000-0000-0000-00000000000a",
          "type": "Test.Param",
          "typeVersion": 2,
          "properties": {
            "record": "Uk9DS1NSRUMBAAAAQVJBUFRTRVQBAAAAAAAAAAIAAAABAAAAIAAAAAAAAABUU0VUAAAAAExFSUYAAAAAAgAAAAQAAAAAAAAABQAAAA=="
          },
          "layout": {
            "x": 100.0,
            "y": -20.0
          }
        },
        {
          "id": "00000000-0000-0000-0000-00000000000b",
          "type": "Test.Param",
          "typeVersion": 2,
          "properties": {
            "record": "Uk9DS1NSRUMBAAAAQVJBUFRTRVQBAAAAAAAAAAIAAAABAAAAIAAAAAAAAABUU0VUAAAAAExFSUYAAAAAAgAAAAQAAAAAAAAABwAAAA=="
          },
          "layout": {
            "x": 110.0,
            "y": -20.0
          }
        },
        {
          "id": "00000000-0000-0000-0000-00000000000c",
          "type": "Test.Add",
          "typeVersion": 1,
          "retainedPins": [
            {
              "key": "Gone",
              "direction": "Input",
              "lastType": 1,
              "value": {
                "record": "Uk9DS1NSRUMBAAAAVUxBVlRTRVQDAAAAAAAAAAEAAAABAAAAIAAAAAAAAABUU0VUAAAAAExFSUYAAAAAAgAAAAQAAAAAAAAAAwAAAA=="
              }
            }
          ],
          "layout": {
            "x": 120.0,
            "y": -20.0
          }
        },
        {
          "id": "00000000-0000-0000-0000-00000000000d",
          "type": "Graph.Reroute",
          "typeVersion": 1,
          "layout": {
            "x": 130.0,
            "y": -20.0
          }
        },
        {
          "id": "00000000-0000-0000-0000-00000000000e",
          "type": "Test.Sequence",
          "typeVersion": 1,
          "layout": {
            "x": 140.0,
            "y": -20.0
          }
        },
        {
          "id": "00000000-0000-0000-0000-00000000000f",
          "type": "Test.Sequence",
          "typeVersion": 1,
          "layout": {
            "x": 150.0,
            "y": -20.0
          }
        },
        {
          "id": "00000000-0000-0000-0000-000000000010",
          "type": "Test.Sequence",
          "typeVersion": 1,
          "layout": {
            "x": 160.0,
            "y": -20.0
          }
        },
        {
          "id": "00000000-0000-0000-0000-000000000011",
          "type": "Test.Toggle",
          "typeVersion": 1,
          "enableState": "Disabled",
          "layout": {
            "x": 170.0,
            "y": -20.0
          }
        },
        {
          "id": "00000000-0000-0000-0000-000000000012",
          "type": "Test.Add",
          "typeVersion": 1,
          "inputDefaults": {
            "A": {
              "record": "Uk9DS1NSRUMBAAAAVUxBVlRTRVQDAAAAAAAAAAEAAAABAAAAIAAAAAAAAABUU0VUAAAAAExFSUYAAAAAAgAAAAQAAAAAAAAAKgAAAA=="
            }
          },
          "layout": {
            "x": 180.0,
            "y": -20.0
          }
        },
        {
          "id": "00000000-0000-0000-0000-000000000013",
          "type": "Test.Dynamic",
          "typeVersion": 1,
          "properties": {
            "record": "Uk9DS1NSRUMBAAAATllORFRTRVQCAAAAAAAAAAIAAAABAAAAIAAAAAAAAABUU0VUAAAAAExFSUYAAAAAAgAAAAQAAAAAAAAAAAAAAA=="
          },
          "layout": {
            "x": 190.0,
            "y": -20.0
          }
        },
        {
          "id": "00000000-0000-0000-0000-000000000014",
          "type": "Test.Missing",
          "typeVersion": 1,
          "layout": {
            "x": 200.0,
            "y": -20.0
          }
        },
        {
          "id": "00000000-0000-0000-0000-000000000015",
          "type": "Graph.Collapsed",
          "typeVersion": 1,
          "layout": {
            "x": 210.0,
            "y": -20.0
          }
        }
      ],
      "edges": [
        {
          "from": [
            "00000000-0000-0000-0000-00000000000a",
            "Value"
          ],
          "to": [
            "00000000-0000-0000-0000-00000000000c",
            "A"
          ]
        },
        {
          "from": [
            "00000000-0000-0000-0000-00000000000d",
            "Out"
          ],
          "to": [
            "00000000-0000-0000-0000-00000000000c",
            "B"
          ]
        },
        {
          "from": [
            "00000000-0000-0000-0000-00000000000b",
            "Value"
          ],
          "to": [
            "00000000-0000-0000-0000-00000000000d",
            "In"
          ]
        },
        {
          "from": [
            "00000000-0000-0000-0000-00000000000e",
            "Then"
          ],
          "to": [
            "00000000-0000-0000-0000-00000000000f",
            "In"
          ]
        },
        {
          "from": [
            "00000000-0000-0000-0000-00000000000e",
            "Then"
          ],
          "to": [
            "00000000-0000-0000-0000-000000000010",
            "In"
          ],
          "order": 1
        },
        {
          "from": [
            "00000000-0000-0000-0000-00000000000c",
            "Result"
          ],
          "to": [
            "00000000-0000-0000-0000-000000000011",
            "In"
          ]
        },
        {
          "from": [
            "00000000-0000-0000-0000-000000000014",
            "Other"
          ],
          "to": [
            "00000000-0000-0000-0000-000000000012",
            "B"
          ]
        },
        {
          "from": [
            "00000000-0000-0000-0000-00000000000a",
            "Value"
          ],
          "to": [
            "00000000-0000-0000-0000-000000000013",
            "Item_1"
          ]
        }
      ],
      "comments": [
        {
          "id": "00000000-0000-0000-0000-000000000028",
          "x": 0.0,
          "y": 0.0,
          "width": 400.0,
          "height": 200.0,
          "text": "",
          "color": [
            1.0,
            0.5,
            0.25,
            1.0
          ],
          "fontSize": 18
        }
      ],
      "bookmarks": [
        {
          "number": 1,
          "x": 0.0,
          "y": 0.0,
          "zoom": 1.0
        },
        {
          "number": 3,
          "x": 0.0,
          "y": 0.0,
          "zoom": 0.5
        }
      ]
    },
    {
      "id": "00000000-0000-0000-0000-000000000002",
      "role": "Collapsed",
      "ownerNode": "00000000-0000-0000-0000-000000000015",
      "nodes": [
        {
          "id": "00000000-0000-0000-0000-00000000001e",
          "type": "Test.Add",
          "typeVersion": 1,
          "layout": {
            "x": 300.0,
            "y": -20.0
          }
        },
        {
          "id": "00000000-0000-0000-0000-00000000001f",
          "type": "Graph.Input",
          "typeVersion": 1,
          "layout": {
            "x": 310.0,
            "y": -20.0
          }
        },
        {
          "id": "00000000-0000-0000-0000-000000000020",
          "type": "Graph.Output",
          "typeVersion": 1,
          "layout": {
            "x": 320.0,
            "y": -20.0
          }
        }
      ],
      "edges": [],
      "comments": [],
      "bookmarks": []
    }
  ]
}
)golden";

} // namespace rocks::nodegraph_tools_test
