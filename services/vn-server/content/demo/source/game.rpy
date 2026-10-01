# Reference "unpacked Ren'Py" source for the demo.
# This is the human-authored side of the pipeline; `renpy2vn.py` converts it
# into ../story.json. Supported subset: label, scene, show/hide, quoted lines
# (with or without a speaker), menu with jump, jump and return.

label start:
    scene bg_hall
    "天快黑了，展厅里只剩下你一个人。"
    "墙上的两件展品，在灯下轻轻地发着光。"
    "你要先看哪一件？"
    menu:
        "会发光的怀表":
            jump watch
        "不会反光的镜子":
            jump mirror

label watch:
    scene bg_watch
    show hero
    "怀表的指针停在同一个时刻，一直没有动。"
    "你把它轻轻放回原处，转身离开。"
    return

label mirror:
    hide hero
    "镜子里没有你的影子，只有一扇小门。"
    "你向前走了一步，门就轻轻打开了。"
    return
