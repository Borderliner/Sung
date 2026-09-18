import QtQuick
import QtQuick.Controls

// Sing along. The lyrics view is for reading; this one is for singing, so it
// answers a different question: not "what are the words" but "where are we in
// them right now". The line being sung fills from its first syllable to its
// last, and the lines around it stand back far enough to be read ahead without
// competing.
//
// Material's display type scale carries the line, one step down for its
// neighbours, and the fill uses the primary role against the same role held at
// reading contrast, so the sweep reads as emphasis rather than as two colours.
Item {
    id: root
    objectName: "singAlong"

    readonly property int activeIndex: app.lyricIndex
    readonly property bool ready: app.lyricLines.length>0 && !app.lyricsBusy
    // Sized from the window so a line lands somewhere near a comfortable
    // measure at any width, then capped so it never outgrows the display scale.
    readonly property int lineSize: Math.max(28,Math.min(72,Math.round(width/22)))
    readonly property int neighbourSize: Math.round(lineSize*0.62)

    ListView {
        id: lines
        objectName: "singAlongLines"
        anchors.fill: parent
        anchors.leftMargin: 48
        anchors.rightMargin: 48
        visible: root.ready
        clip: true
        spacing: Math.round(root.lineSize*0.5)
        model: app.lyricLines
        reuseItems: true
        cacheBuffer: 200
        boundsBehavior: Flickable.StopAtBounds
        interactive: false
        // Padding above and below the words, so even a short lyric can bring
        // its first and last lines to the place the eye is already looking.
        topMargin: height*0.34
        bottomMargin: height*0.5
        currentIndex: root.activeIndex
        // The sung line sits just above centre, which leaves more of what is
        // coming visible than what has gone.
        preferredHighlightBegin: height*0.38
        preferredHighlightEnd: height*0.46
        highlightRangeMode: ListView.ApplyRange
        highlightMoveDuration: app.motion?420:0
        highlight: Item {}

        function centre() {
            Qt.callLater(function(){ if(lines.visible && root.activeIndex>=0) lines.positionViewAtIndex(root.activeIndex,ListView.Center); });
        }
        Component.onCompleted: centre()
        onCountChanged: centre()
        onVisibleChanged: if(visible)centre()

        delegate: Item {
            id: line
            required property var modelData
            required property int index
            readonly property bool current: index===root.activeIndex
            readonly property bool past: root.activeIndex>=0 && index<root.activeIndex
            width: lines.width
            implicitHeight: body.implicitHeight

            // Everything but the line being sung stands back; what has already
            // been sung stands back furthest.
            opacity: current ? 1 : past ? 0.28 : 0.45
            Behavior on opacity { enabled: app.motion; NumberAnimation { duration: Theme.normal; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.effectsCurve } }

            Text {
                id: body
                objectName: line.current ? "singAlongCurrent" : "singAlongLine"
                width: parent.width
                text: line.modelData.text || "…"
                font.family: Theme.fontFamily
                font.pixelSize: line.current ? root.lineSize : root.neighbourSize
                font.weight: line.current ? Font.DemiBold : Font.Medium
                Behavior on font.pixelSize { enabled: app.motion; NumberAnimation { duration: 320; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.curve } }
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                lineHeight: 1.2
                // Unsung text is the same colour held at reading contrast, so
                // the fill below reads as the line being consumed.
                color: Theme.text
            }

            // The fill: the same line, in the accent colour, revealed from the
            // left exactly as far as playback has travelled through it.
            Item {
                objectName: "singAlongFill"
                anchors.left: body.left
                anchors.top: body.top
                height: body.height
                width: line.current ? body.width*Math.max(0,app.lyricProgress) : line.past ? body.width : 0
                visible: line.current || line.past
                clip: true
                Text {
                    width: body.width
                    text: body.text
                    font: body.font
                    horizontalAlignment: body.horizontalAlignment
                    wrapMode: body.wrapMode
                    lineHeight: body.lineHeight
                    color: Theme.primary
                }
            }
        }
    }

    // A countdown through instrumental stretches, so a long gap does not read
    // as the words having stopped working.
    Column {
        objectName: "singAlongWaiting"
        anchors.centerIn: parent
        spacing: 12
        visible: root.ready && root.activeIndex<0
        SungText {
            objectName: "singAlongCue"
            anchors.horizontalCenter: parent.horizontalCenter
            text: app.lyricGapSeconds>0 ? "Lyrics in "+app.lyricGapSeconds+" s" : "♪"
            color: Theme.muted
            font.pixelSize: Math.round(root.neighbourSize*0.7)
        }
    }

    MBusyIndicator { anchors.centerIn: parent; running: app.lyricsBusy; label: "Loading lyrics" }
    SungText {
        objectName: "singAlongUnavailable"
        anchors.centerIn: parent
        visible: !root.ready && !app.lyricsBusy
        text: "No timed lyrics for this song"
        color: Theme.muted
        font.pixelSize: Theme.bodyLarge
    }
}
