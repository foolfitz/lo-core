# Writer Text Editor Sidebar Extension — 研究報告

## Context

開發一個 LibreOffice Writer Python Extension（.oxt），功能為：
- 在 sidebar 新增一個文字編輯面板（含多行輸入框）
- 使用者選取段落後，透過**右鍵選單**將選取文字送入 sidebar 輸入框
- 輸入框下方有「貼上」和「取代」兩個按鈕
- 「貼上」：在選取區的下一段新增段落
- 「取代」：刪除原選取文字，以輸入框文字取代
- 兩種操作都保留原始段落樣式（ParaStyleName）
- 多段落選取時整體處理；字元格式不需保留

---

## 1. Extension 檔案結構

```
writer-text-editor-ext/
├── META-INF/
│   └── manifest.xml              # 註冊 .component 和所有 .xcu
├── dialogs/
│   └── editor_panel.xdl          # Sidebar 面板 UI 定義
├── description.xml               # Extension metadata + identifier
├── text_editor.py                # 主程式（所有 Python 類別）
├── text_editor.component         # UNO 元件描述檔
├── Factory.xcu                   # 註冊 UIElementFactory
├── Sidebar.xcu                   # 定義 Deck 和 Panel
└── registration/
    └── license.txt
```

### 關鍵名稱配對

| 來源 | 欄位 | 必須一致於 |
|------|------|-----------|
| `description.xml` → `<identifier value="X"/>` | ↔ | Python 的 `EXTENSION_ID` 常數 |
| `.component` → `<implementation name="X">` | ↔ | `Factory.xcu` → `FactoryImplementation` |
| `Factory.xcu` → `Name` 值 | ↔ | `Sidebar.xcu` → `ImplementationURL` 中的 factory 名稱 |
| `Sidebar.xcu` → `PanelList.DeckId` | ↔ | `Sidebar.xcu` → `DeckList.Id` |
| `.component` → 第二個 `<implementation>` | ↔ | `service:` URL 中的 service name |

---

## 2. 架構設計

### 2.1 元件總覽

Python 模組中包含以下類別和一個模組級共享變數：

```
text_editor.py
├── _active_panel = None                    # 模組級共享變數（ToolPanel 參考）
│
├── EditorSidebarFactory (XUIElementFactory)
│   └── createUIElement() → EditorUIElement
│
├── EditorUIElement (XUIElement)
│   └── getRealInterface() → EditorToolPanel
│
├── EditorToolPanel (XToolPanel)
│   ├── 持有 panel window 和控制項參考
│   ├── 註冊 XContextMenuInterceptor
│   ├── set_text(text, style) — 從右鍵選單呼叫
│   ├── _on_paste() — 貼上按鈕處理
│   └── _on_replace() — 取代按鈕處理
│
├── ContextMenuInterceptor (XContextMenuInterceptor)
│   └── notifyContextMenuExecute() — 加入右鍵選單項目
│
├── GrabSelectionJob (XJobExecutor)
│   └── trigger() — 右鍵選單項目觸發時呼叫
│
├── PasteButtonListener (XActionListener)
├── ReplaceButtonListener (XActionListener)
└── ResizeListener (XWindowListener)
```

### 2.2 資料流

```
使用者選取文字 → 右鍵 → 「送到編輯面板」
    │
    ▼
ContextMenuInterceptor.notifyContextMenuExecute()
    │  建立選單項目 CommandURL = "service:org.example.text-editor.job?GrabSelection"
    ▼
使用者點擊選單項目
    │
    ▼
GrabSelectionJob.trigger("GrabSelection")
    │  取得當前選取的文字和段落樣式
    │  呼叫 _active_panel.set_text(text, style)
    ▼
Sidebar 輸入框顯示選取的文字
    │
    ▼
使用者編輯後點「貼上」或「取代」
    │
    ├─「貼上」→ 在原選取區末尾插入新段落 + 套用原始樣式
    └─「取代」→ 用 setString() 取代原選取文字 + 還原樣式
```

### 2.3 元件註冊

`.component` 檔案需註冊兩個實作：

```xml
<components xmlns="http://openoffice.org/2010/uno-components">
  <component loader="com.sun.star.loader.Python" uri="./text_editor.py">
    <!-- Sidebar Factory -->
    <implementation name="org.example.text-editor.SidebarFactory">
      <service name="org.example.text-editor.SidebarFactory"/>
    </implementation>
    <!-- Job for service: URL handling -->
    <implementation name="org.example.text-editor.job">
      <service name="org.example.text-editor.job"/>
    </implementation>
  </component>
</components>
```

---

## 3. 右鍵選單整合（XContextMenuInterceptor）

### 3.1 介面定義

來源：`offapi/com/sun/star/ui/XContextMenuInterceptor.idl`

```
interface XContextMenuInterceptor {
    ContextMenuInterceptorAction notifyContextMenuExecute(ContextMenuExecuteEvent aEvent);
};
```

`ContextMenuExecuteEvent` 包含：
- `ActionTriggerContainer`：可修改的選單容器（同時是 `XMultiServiceFactory`）
- `Selection`：當前選取（`XSelectionSupplier`）
- `SourceWindow`：來源視窗
- `ExecutePosition`：滑鼠位置

### 3.2 註冊方式

透過 `XContextMenuInterception` 介面（由 `XController` / `SfxBaseController` 暴露）：

```python
# 註冊
controller = doc.getCurrentController()
controller.registerContextMenuInterceptor(interceptor)

# 釋放
controller.releaseContextMenuInterceptor(interceptor)
```

來源：`sfx2/source/view/sfxbasecontroller.cxx`（行 997-1015）

### 3.3 建立選單項目

來源：Java 範例 `odk/examples/DevelopersGuide/OfficeDev/ContextMenuInterceptor.java`

```python
def notifyContextMenuExecute(self, aEvent):
    xContextMenu = aEvent.ActionTriggerContainer

    # ActionTriggerContainer 本身就是 XMultiServiceFactory
    # 建立分隔線
    xSeparator = xContextMenu.createInstance("com.sun.star.ui.ActionTriggerSeparator")

    # 建立選單項目
    xMenuEntry = xContextMenu.createInstance("com.sun.star.ui.ActionTrigger")
    xMenuEntry.setPropertyValue("Text", "送到編輯面板")
    xMenuEntry.setPropertyValue("CommandURL",
        "service:org.example.text-editor.job?GrabSelection")

    # 插入到選單末尾
    count = xContextMenu.getCount()
    xContextMenu.insertByIndex(count, xSeparator)
    xContextMenu.insertByIndex(count + 1, xMenuEntry)

    return ContextMenuInterceptorAction.EXECUTE_MODIFIED
```

### 3.4 service: URL 觸發機制

`service:` URL 由 `framework/source/dispatch/servicehandler.cxx` 處理：
- 建立指定名稱的 service 實例
- 呼叫 `XJobExecutor.trigger(args)`，其中 `args` 是 `?` 之後的字串

因此 `CommandURL = "service:org.example.text-editor.job?GrabSelection"` 會：
1. 建立 `org.example.text-editor.job` service
2. 呼叫 `trigger("GrabSelection")`

### 3.5 條件顯示

只有在選取了文字時才顯示選單項目：

```python
def notifyContextMenuExecute(self, aEvent):
    # 檢查是否有文字選取
    try:
        selection = aEvent.Selection.getSelection()
        if not selection.supportsService("com.sun.star.text.TextRanges"):
            return ContextMenuInterceptorAction.IGNORED
        if selection.getCount() == 0:
            return ContextMenuInterceptorAction.IGNORED
        text_range = selection.getByIndex(0)
        if not text_range.getString():
            return ContextMenuInterceptorAction.IGNORED
    except:
        return ContextMenuInterceptorAction.IGNORED

    # 有文字選取，加入選單項目...
```

---

## 4. Sidebar 面板實作

### 4.1 三層類別架構

來源：`odk/examples/python/toolpanel/toolpanel.py`

```
EditorSidebarFactory (XUIElementFactory)
  └─ createUIElement(url, properties)
       ├─ 從 properties 取出 Frame, ParentWindow
       └─ 建立 EditorUIElement

EditorUIElement (XUIElement)
  └─ getRealInterface()
       ├─ 用 ContainerWindowProvider 載入 .xdl
       └─ 建立 EditorToolPanel

EditorToolPanel (XToolPanel)
  ├─ Window / PanelWindow 屬性（必要）
  ├─ 取得控制項參考（EditContent, BtnPaste, BtnReplace）
  ├─ 設定按鈕事件 listener
  ├─ 設定 resize listener
  ├─ 註冊 ContextMenuInterceptor
  └─ createAccessible() → 返回 panel window
```

### 4.2 Sidebar.xcu 設定

需定義 Deck（sidebar 頁籤）和 Panel（頁籤內容）：

```xml
<node oor:name="DeckList">
  <node oor:name="TextEditorDeck" oor:op="replace">
    <prop oor:name="Title"><value xml:lang="en-US">Text Editor</value></prop>
    <prop oor:name="Id"><value>TextEditorDeck</value></prop>
    <prop oor:name="ContextList">
      <value oor:separator=";">WriterVariants, any, visible ;</value>
    </prop>
    <prop oor:name="OrderIndex"><value>900</value></prop>
  </node>
</node>

<node oor:name="PanelList">
  <node oor:name="TextEditorPanel" oor:op="replace">
    <prop oor:name="DeckId"><value>TextEditorDeck</value></prop>
    <prop oor:name="ImplementationURL">
      <value>private:resource/toolpanel/TextEditorFactory/TextEditorPanel</value>
    </prop>
    ...
  </node>
</node>
```

`ImplementationURL` 格式：`private:resource/toolpanel/<Factory.xcu的Name>/<PanelId>`

### 4.3 UI 定義（.xdl）

```xml
<dlg:window dlg:id="EditorPanel" dlg:width="200" dlg:height="300"
            dlg:closeable="false" dlg:moveable="false" dlg:withtitlebar="false">
  <dlg:bulletinboard>
    <dlg:textfield dlg:id="EditContent"
                   dlg:left="4" dlg:top="4"
                   dlg:width="192" dlg:height="240"
                   dlg:multiline="true" dlg:vscroll="true"/>
    <dlg:button dlg:id="BtnPaste"
                dlg:left="4" dlg:top="250"
                dlg:width="92" dlg:height="20"
                dlg:value="貼上"/>
    <dlg:button dlg:id="BtnReplace"
                dlg:left="100" dlg:top="250"
                dlg:width="92" dlg:height="20"
                dlg:value="取代"/>
  </dlg:bulletinboard>
</dlg:window>
```

初始尺寸不重要，resize listener 會動態調整。

### 4.4 Resize 處理

來源：附件指南 §3.9

Sidebar 寬度可變，必須實作 `XWindowListener`：

```python
class ResizeListener(unohelper.Base, XWindowListener):
    def windowResized(self, event):
        w, h = event.Width, event.Height
        # 輸入框：佔滿上方空間
        edit_h = max(h - MARGIN*3 - BUTTON_HEIGHT, 20)
        edit.setPosSize(MARGIN, MARGIN, w - MARGIN*2, edit_h, POSSIZE)
        # 兩個按鈕：並排在底部
        btn_w = (w - MARGIN*3) // 2
        btn_paste.setPosSize(MARGIN, MARGIN + edit_h + MARGIN, btn_w, BUTTON_HEIGHT, POSSIZE)
        btn_replace.setPosSize(MARGIN*2 + btn_w, MARGIN + edit_h + MARGIN, btn_w, BUTTON_HEIGHT, POSSIZE)
```

---

## 5. 文字操作 API

### 5.1 取得選取內容與段落樣式

來源：`odk/examples/python/Text/WriterSelector.py`、`sw/qa/uitest/navigator/tdf154521.py`

```python
controller = doc.getCurrentController()
selection = controller.getSelection()

if selection.supportsService("com.sun.star.text.TextRanges"):
    # 取得整個選取範圍的文字
    all_text = ""
    for i in range(selection.getCount()):
        text_range = selection.getByIndex(i)
        all_text += text_range.getString()

    # 取得第一段的段落樣式
    first_range = selection.getByIndex(0)
    para_style = first_range.getPropertyValue("ParaStyleName")
```

### 5.2 取代操作（保留段落樣式）

來源：`wizards/com/sun/star/wizards/agenda/AgendaDocument.py`

```python
def do_replace(doc, new_text, saved_style):
    controller = doc.getCurrentController()
    selection = controller.getSelection()
    if selection.supportsService("com.sun.star.text.TextRanges"):
        text_range = selection.getByIndex(0)
        # 儲存樣式 → 取代文字 → 還原樣式
        text_range.setString(new_text)
        text_range.setPropertyValue("ParaStyleName", saved_style)
```

### 5.3 貼上操作（在選取區後新增段落）

來源：`odk/examples/python/Text/SWriter.py`

```python
from com.sun.star.text.ControlCharacter import PARAGRAPH_BREAK

def do_paste(doc, new_text, saved_style):
    text = doc.getText()
    controller = doc.getCurrentController()
    selection = controller.getSelection()

    if selection.supportsService("com.sun.star.text.TextRanges"):
        last_range = selection.getByIndex(selection.getCount() - 1)
        # 在選取區末尾建立 cursor
        cursor = text.createTextCursorByRange(last_range.getEnd())
        # 插入段落分隔符
        text.insertControlCharacter(cursor, PARAGRAPH_BREAK, False)
        # 套用段落樣式
        cursor.setPropertyValue("ParaStyleName", saved_style)
        # 插入文字
        text.insertString(cursor, new_text, False)
```

### 5.4 多段落處理

選取跨多段落時，`getString()` 會包含所有段落的文字（段落間以換行分隔）。操作時：
- **取代**：`selection.getByIndex(0).setString(new_text)` 會取代整個選取範圍
- **貼上**：在最後一段之後插入

---

## 6. 模組間通訊

### 挑戰

`GrabSelectionJob` 和 `EditorToolPanel` 是獨立的 UNO 元件實例，需要共享狀態。

### 解決方案：模組級變數

```python
# text_editor.py 頂層
_active_panel = None  # 由 EditorToolPanel 建構時設定

class EditorToolPanel(unohelper.Base, XToolPanel):
    def __init__(self, ctx, panel_window):
        global _active_panel
        _active_panel = self
        # ...

class GrabSelectionJob(unohelper.Base, XJobExecutor):
    def trigger(self, args):
        global _active_panel
        if args == "GrabSelection" and _active_panel:
            # 取得選取文字和樣式
            # 呼叫 _active_panel.set_text(text, style)
```

**注意**：Python loader 在同一 process 中共享模組命名空間，所以模組級變數在同一個 LibreOffice process 的不同元件實例間是共享的。

---

## 7. 打包與安裝

```bash
cd writer-text-editor-ext/
zip -r ../writer-text-editor.oxt ./*

# 安裝
unopkg add writer-text-editor.oxt
# 強制更新
unopkg add --force writer-text-editor.oxt
# 移除（用 description.xml 的 identifier）
unopkg remove org.example.writer-text-editor

# 重啟 LibreOffice 使變更生效
```

---

## 8. 已知風險與注意事項

1. **service: URL 在 ActionTrigger 中的相容性**：`service:` 協定由 `framework/source/dispatch/servicehandler.cxx` 處理，理論上可在 ActionTrigger 的 CommandURL 中使用，但官方範例（Java）都使用 `.uno:` 命令。若 `service:` 不行，備案是用 `.uno:` dispatch + `XDispatchProvider` 攔截。

2. **Python UNO 中的 XMultiServiceFactory 存取**：Java 範例中 ActionTriggerContainer 直接透過 `queryInterface` 取得 `XMultiServiceFactory`。在 Python 中，由於 `ActionTriggerContainer` service 已宣告實作此介面，應可直接呼叫 `xContextMenu.createInstance(...)`。

3. **Interceptor 生命週期**：Interceptor 是 per-controller 的。需在 sidebar panel 建立時註冊，dispose 時釋放。如果使用者開多個 Writer 文件，每個都需要獨立的 interceptor。

4. **選取範圍保持**：使用者從右鍵選單觸發「送到編輯面板」後，編輯 sidebar 中的文字時，原始的文件選取可能已改變。需要在 `GrabSelectionJob.trigger()` 中儲存選取的 `XTextRange` 參考，供後續「貼上」/「取代」使用。

5. **模組級變數限制**：`_active_panel` 只能記錄一個 panel 實例。若使用者同時開啟多個 Writer 視窗，需改用 dict 以 document 為 key。

---

## 9. 參考檔案索引

| 用途 | 檔案路徑 |
|------|----------|
| Sidebar 完整範例 | `odk/examples/python/toolpanel/` |
| 簡易 Extension 範例 | `odk/examples/python/minimal-extension/` |
| 文字操作範例 | `odk/examples/python/Text/SWriter.py` |
| 選取偵測範例 | `odk/examples/python/Text/WriterSelector.py` |
| 樣式建立範例 | `odk/examples/python/Text/StyleCreation.py` |
| XSelectionChangeListener 實例 | `sw/qa/uitest/navigator/tdf154521.py` |
| Sidebar 設定參考 | `officecfg/registry/data/org/openoffice/Office/UI/Sidebar.xcu` |
| XContextMenuInterceptor Java 範例 | `odk/examples/DevelopersGuide/OfficeDev/ContextMenuInterceptor.java` |
| ActionTrigger IDL | `offapi/com/sun/star/ui/ActionTrigger.idl` |
| ContextMenuExecuteEvent IDL | `offapi/com/sun/star/ui/ContextMenuExecuteEvent.idl` |
| service: URL handler 實作 | `framework/source/dispatch/servicehandler.cxx` |
| SfxBaseController（interceptor 註冊） | `sfx2/source/view/sfxbasecontroller.cxx` |
| 附件開發指南 | `libreoffice-python-extension-guide.md` |
