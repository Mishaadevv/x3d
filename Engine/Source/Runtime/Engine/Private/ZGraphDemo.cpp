// The script graphs of the Third Person template's mini game (the ZGraph acceptance demo): coins to collect, a gate
// that opens with enough of them, and the HUD with a pause menu - all logic as nodes, no ZeqouScript.

#include "Engine/ZGraph.h"

namespace zx {

namespace {

/// Builds graphs in code: nodes laid out on a grid (columns 270 apart, rows 120).
struct Builder
{
    Graph g;

    u32 Node(const char* type, f32 col, f32 row)
    {
        GraphNode n;
        n.id = g.NextId();
        n.type = type;
        n.position = Vec2(col * 270.0f, row * 120.0f);
        g.nodes.PushBack(move(n));
        return g.nodes.Back().id;
    }
    u32 Value(u32 id, const char* pin, const char* value)
    {
        g.FindNode(id)->values.InsertOrAssign(pin, value);
        return id;
    }
    u32 Setting(u32 id, const char* key, const char* value)
    {
        g.FindNode(id)->settings.InsertOrAssign(key, value);
        return id;
    }
    void Link(u32 a, const char* ap, u32 b, const char* bp) { g.links.PushBack(GraphLink{a, ap, b, bp}); }
    /// action.* with an exec input: wired after `prev`.`pin`.
    u32 Then(u32 prev, const char* pin, const char* type, f32 col, f32 row)
    {
        const u32 id = Node(type, col, row);
        Link(prev, pin, id, "in");
        return id;
    }
};

} // namespace

Graph MakeCoinGraph()
{
    // Spins; when the player comes close: +1 "coins", a sound, gone.
    Builder b;
    b.g.name = "Coin";
    b.g.variables.PushBack(GraphVariable{"spinSpeed", GraphPinType::Number, "180", true, "Degrees per second"});
    b.g.variables.PushBack(GraphVariable{"pickupDistance", GraphPinType::Number, "1.4", true, "Meters from the player's feet"});
    const u32 update = b.Node("event.update", 0, 0);
    const u32 speed = b.Setting(b.Node("pure.getVariable", 0, 1), "variable", "spinSpeed");
    const u32 mul = b.Node("pure.multiply", 1, 1);
    b.Link(speed, "value", mul, "a");
    b.Link(update, "dt", mul, "b");
    const u32 spinVec = b.Node("pure.makeVec3", 2, 1);
    b.Link(mul, "result", spinVec, "y");
    const u32 rotate = b.Then(update, "then", "action.rotate", 3, 0);
    b.Link(spinVec, "vector", rotate, "degrees");
    // Is the player close?
    const u32 self = b.Node("pure.getPosition", 0, 3);
    const u32 player = b.Value(b.Node("pure.find", 0, 4), "name", "Player");
    const u32 playerPos = b.Node("pure.getPosition", 1, 4);
    b.Link(player, "entity", playerPos, "target");
    const u32 dist = b.Node("pure.distance", 2, 3);
    b.Link(self, "world", dist, "a");
    b.Link(playerPos, "world", dist, "b");
    const u32 range = b.Setting(b.Node("pure.getVariable", 2, 5), "variable", "pickupDistance");
    const u32 near = b.Node("pure.less", 3, 3);
    b.Link(dist, "distance", near, "a");
    b.Link(range, "value", near, "b");
    const u32 valid = b.Node("pure.isValid", 1, 5);
    b.Link(player, "entity", valid, "value");
    const u32 both = b.Node("pure.and", 4, 3); // no player in the scene: no distance either
    b.Link(valid, "valid", both, "a");
    b.Link(near, "result", both, "b");
    const u32 branch = b.Then(rotate, "then", "flow.branch", 5, 0);
    b.Link(both, "result", branch, "condition");
    const u32 add = b.Value(b.Value(b.Then(branch, "true", "action.addGameValue", 6, 0), "key", "coins"), "amount", "1");
    const u32 sound = b.Value(b.Then(add, "then", "action.playSoundAt", 7, 0), "sound", "StarterContent/Sounds/Props/Coins");
    b.Link(self, "world", sound, "position");
    b.Then(sound, "then", "action.destroy", 8, 0);
    return b.g;
}

Graph MakeGateGraph()
{
    // Press E near the gate: with enough coins it slides up, otherwise the HUD says how many are needed.
    Builder b;
    b.g.name = "Gate";
    b.g.variables.PushBack(GraphVariable{"coinsNeeded", GraphPinType::Number, "5", true, "Coins the player must collect"});
    b.g.variables.PushBack(GraphVariable{"isOpen", GraphPinType::Bool, "false", false, {}});
    // A hint when the game starts.
    const u32 start = b.Node("event.start", 0, 0);
    const u32 hintStart = b.Value(b.Node("pure.translate", 0, 1), "key", "hint.start");
    const u32 show = b.Value(b.Then(start, "then", "action.setText", 1, 0), "target", "Hint");
    b.Link(hintStart, "text", show, "text");
    const u32 wait = b.Value(b.Then(show, "then", "flow.delay", 2, 0), "seconds", "6");
    b.Value(b.Value(b.Then(wait, "completed", "action.setText", 3, 0), "target", "Hint"), "text", "");
    // E near the gate, while it's closed.
    const u32 key = b.Setting(b.Node("event.keyPressed", 0, 3), "key", "E");
    const u32 self = b.Node("pure.getPosition", 0, 4);
    const u32 player = b.Value(b.Node("pure.find", 0, 5), "name", "Player");
    const u32 playerPos = b.Node("pure.getPosition", 1, 5);
    b.Link(player, "entity", playerPos, "target");
    const u32 dist = b.Node("pure.distance", 2, 4);
    b.Link(self, "world", dist, "a");
    b.Link(playerPos, "world", dist, "b");
    const u32 near = b.Value(b.Node("pure.less", 3, 4), "b", "5"); // meters from the gate's center
    b.Link(dist, "distance", near, "a");
    const u32 valid = b.Node("pure.isValid", 1, 6);
    b.Link(player, "entity", valid, "value");
    const u32 here = b.Node("pure.and", 4, 4);
    b.Link(valid, "valid", here, "a");
    b.Link(near, "result", here, "b");
    const u32 open = b.Setting(b.Node("pure.getVariable", 3, 6), "variable", "isOpen");
    const u32 closed = b.Node("pure.not", 4, 6);
    b.Link(open, "value", closed, "value");
    const u32 can = b.Node("pure.and", 5, 5);
    b.Link(here, "result", can, "a");
    b.Link(closed, "result", can, "b");
    const u32 check = b.Then(key, "then", "flow.branch", 5, 3);
    b.Link(can, "result", check, "condition");
    // Enough coins?
    const u32 coins = b.Value(b.Node("pure.gameValue", 5, 7), "key", "coins");
    const u32 needed = b.Setting(b.Node("pure.getVariable", 5, 8), "variable", "coinsNeeded");
    const u32 enough = b.Node("pure.greaterEqual", 6, 7);
    b.Link(coins, "value", enough, "a");
    b.Link(needed, "value", enough, "b");
    const u32 decide = b.Then(check, "true", "flow.branch", 6, 3);
    b.Link(enough, "result", decide, "condition");
    // Not yet: "Coins needed: 5".
    const u32 needText = b.Value(b.Node("pure.translate", 7, 8), "key", "hint.need");
    const u32 needJoin = b.Node("pure.concat", 8, 8);
    b.Link(needText, "text", needJoin, "a");
    b.Link(needed, "value", needJoin, "b");
    const u32 latch = b.Value(b.Then(decide, "false", "action.playSoundAt", 7, 5), "sound", "StarterContent/Sounds/Props/Latch");
    b.Link(self, "world", latch, "position");
    const u32 say = b.Value(b.Then(latch, "then", "action.setText", 9, 5), "target", "Hint");
    b.Link(needJoin, "text", say, "text");
    // Open: slide up, a sound, "The gate opens!", then the hint clears.
    const u32 mark = b.Value(b.Setting(b.Then(decide, "true", "action.setVariable", 7, 3), "variable", "isOpen"), "value", "true");
    const u32 up = b.Value(b.Node("pure.addVec3", 7, 2), "b", "0 3.4 0");
    b.Link(self, "position", up, "a");
    const u32 slide = b.Value(b.Then(mark, "then", "action.moveTo", 8, 3), "seconds", "1.6");
    b.Link(up, "vector", slide, "position");
    const u32 creak = b.Value(b.Then(slide, "then", "action.playSoundAt", 9, 3), "sound", "StarterContent/Sounds/Props/Door_Open");
    b.Link(self, "world", creak, "position");
    const u32 opensText = b.Value(b.Node("pure.translate", 9, 2), "key", "hint.open");
    const u32 tell = b.Value(b.Then(creak, "then", "action.setText", 10, 3), "target", "Hint");
    b.Link(opensText, "text", tell, "text");
    const u32 later = b.Value(b.Then(tell, "then", "flow.delay", 11, 3), "seconds", "3");
    b.Value(b.Value(b.Then(later, "completed", "action.setText", 12, 3), "target", "Hint"), "text", "");
    return b.g;
}

Graph MakeGameHUDGraph()
{
    // The coin counter, and a pause menu on Tab: Resume (also Escape / gamepad B) and a language switch.
    Builder b;
    b.g.name = "GameHUD";
    b.g.variables.PushBack(GraphVariable{"paused", GraphPinType::Bool, "false", false, {}});
    // Counter: "Coins: 3".
    const u32 update = b.Node("event.update", 0, 0);
    const u32 label = b.Value(b.Node("pure.translate", 0, 1), "key", "hud.coins");
    const u32 colon = b.Value(b.Node("pure.concat", 1, 1), "b", ": ");
    b.Link(label, "text", colon, "a");
    const u32 coins = b.Value(b.Node("pure.gameValue", 1, 2), "key", "coins");
    const u32 text = b.Node("pure.concat", 2, 1);
    b.Link(colon, "text", text, "a");
    b.Link(coins, "value", text, "b");
    const u32 set = b.Value(b.Then(update, "then", "action.setText", 3, 0), "target", "Coins");
    b.Link(text, "text", set, "text");
    // The menu starts hidden.
    const u32 start = b.Node("event.start", 0, 3);
    b.Value(b.Value(b.Then(start, "then", "action.setVisible", 1, 3), "target", "Pause Menu"), "visible", "false");
    // Tab toggles.
    const u32 tab = b.Setting(b.Node("event.keyPressed", 0, 5), "key", "Tab");
    const u32 paused = b.Setting(b.Node("pure.getVariable", 0, 6), "variable", "paused");
    const u32 which = b.Then(tab, "then", "flow.branch", 1, 5);
    b.Link(paused, "value", which, "condition");
    b.Setting(b.Then(which, "true", "action.callEvent", 2, 5), "event", "ResumeGame");
    b.Setting(b.Then(which, "false", "action.callEvent", 2, 6), "event", "PauseGame");
    // Pause: menu, mouse, frozen time, focus on Resume (gamepad).
    const u32 pause = b.Setting(b.Node("event.custom", 0, 8), "event", "PauseGame");
    u32 p = b.Value(b.Setting(b.Then(pause, "then", "action.setVariable", 1, 8), "variable", "paused"), "value", "true");
    p = b.Value(b.Value(b.Then(p, "then", "action.setVisible", 2, 8), "target", "Pause Menu"), "visible", "true");
    p = b.Value(b.Then(p, "then", "action.showCursor", 3, 8), "show", "true");
    p = b.Value(b.Then(p, "then", "action.timeScale", 4, 8), "scale", "0");
    b.Value(b.Then(p, "then", "action.focus", 5, 8), "target", "Resume");
    // Resume.
    const u32 resume = b.Setting(b.Node("event.custom", 0, 10), "event", "ResumeGame");
    u32 r = b.Value(b.Setting(b.Then(resume, "then", "action.setVariable", 1, 10), "variable", "paused"), "value", "false");
    r = b.Value(b.Value(b.Then(r, "then", "action.setVisible", 2, 10), "target", "Pause Menu"), "visible", "false");
    r = b.Value(b.Then(r, "then", "action.showCursor", 3, 10), "show", "false");
    b.Value(b.Then(r, "then", "action.timeScale", 4, 10), "scale", "1");
    // Buttons.
    const u32 clickResume = b.Setting(b.Node("event.uiClick", 0, 12), "widget", "Resume");
    b.Setting(b.Then(clickResume, "then", "action.callEvent", 1, 12), "event", "ResumeGame");
    const u32 back = b.Node("event.uiBack", 0, 13);
    b.Setting(b.Then(back, "then", "action.callEvent", 1, 13), "event", "ResumeGame");
    const u32 clickLang = b.Setting(b.Node("event.uiClick", 0, 14), "widget", "Language");
    const u32 flip = b.Then(clickLang, "then", "flow.flipFlop", 1, 14);
    b.Value(b.Then(flip, "a", "action.setLanguage", 2, 14), "code", "ru");
    b.Value(b.Then(flip, "b", "action.setLanguage", 2, 15), "code", "en");
    return b.g;
}

} // namespace zx
