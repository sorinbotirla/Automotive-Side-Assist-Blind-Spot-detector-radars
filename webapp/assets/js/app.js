var RadarLogger = function(){
    var _self = this;

    _self.waiters = {};
    _self.repeaters = {};

    _self.state = {
        logging: 0,
        file: "",
        viewMode: 0,
        logName: "",
        offset: 0,
        limit: 1000,
        lastCount: 0,
        isSettingsPage: 0
    };

    _self.colors = {
        hbLeft:  "red",
        hbRight: "green",
        rcwlLeft:"blue",
        rcwlRight:"orange"
    };

    _self.graphs = {
        hbLeft: null,
        hbRight: null,
        rcwlLeft: null,
        rcwlRight: null
    };

    _self.shading = {
        left: [],
        right: []
    };

    _self.pending = {
        seq: 0,
        latestSeqByKey: {},
        timersByKey: {}
    };

    _self.settingsCache = {};
    _self.uiLoading = 0;

    _self.liveTimer = null;

    // Canonical keys + legacy aliases (read + send mapping)
    _self.keyAliases = {
        // noise update speed rename
        "NOISE_ALPHA_SHIFT": "NOISE_AVERAGE_UPDATE_SPEED",
        // ms renames
        "MOTION_HOLD_MS": "MOTION_HOLD_MILISECONDS",
        "RCWL_MIN_ACTIVE_MS": "RCWL_MIN_ACTIVE_MILISECONDS",
        // us renames
        "SAMPLE_PERIOD_US": "SAMPLE_PERIOD_MICROSECONDS",
        "MIN_PERIOD_US": "MIN_PERIOD_MICROSECONDS",
        "MAX_PERIOD_US": "MAX_PERIOD_MICROSECONDS"
    };

    _self.qs = function(name){
        var re = new RegExp("[?&]" + name + "=([^&]+)"),
            m = re.exec(window.location.search);
        return m ? decodeURIComponent(m[1]) : "";
    };

    _self.apiGet = function(url, cb){
        $.get(url)
            .done(function(data, textStatus, jqXHR){
                var txt = (typeof data === "string") ? data : JSON.stringify(data);
                cb(jqXHR.status, txt);
            })
            .fail(function(jqXHR){
                cb(jqXHR.status || 0, "");
            });
    };

    _self.setStatus = function(msg, isError){
        var el = $("#statusLine");
        if (!el.length) return;
        el.text(msg);
        el.attr("class", isError ? "status error" : "status ok");
    };

    _self.setAck = function(msg){
        var el = $("#ackLine");
        if (!el.length) return;
        el.text("ACK: " + (msg || "none"));
    };

    _self.showNav = function(){
        var prev = $("#prevChunk"),
            next = $("#nextChunk"),
            meta = $("#chunkMeta"),
            canPrev = (_self.state.offset > 0),
            canNext = (_self.state.lastCount >= _self.state.limit);

        if (prev.length) {
            if (canPrev) prev.show();
            else prev.hide();
        }

        if (next.length) {
            if (canNext) next.show();
            else next.hide();
        }

        if (meta.length) {
            meta.text("file=" + _self.state.logName + " offset=" + _self.state.offset + " count=" + _self.state.lastCount);
        }
    };

    _self.normalizeKeyForSend = function(key){
        if (_self.keyAliases[key]) return _self.keyAliases[key];
        return key;
    };

    _self.isValidIntString = function(v){
        // allow "0", "-1", "123"; disallow "" or "-" or "12a"
        return (typeof v === "string") && (/^-?\d+$/.test($.trim(v)));
    };

    _self.getFieldValue = function($el){
        if (!$el || !$el.length) return "";
        return $el.val();
    };

    _self.setFieldValueIfExists = function(id, val){
        var $el = $("#" + id);
        if (!$el.length) return;
        $el.val(val);
    };

    _self.setSelectBoolIfExists = function(id, boolVal){
        var $el = $("#" + id);
        if (!$el.length) return;
        $el.val(boolVal ? "true" : "false");
    };

    _self.pickFirst = function(o, keys){
        var i = 0, k = "";
        for(i=0;i<keys.length;i++){
            k = keys[i];
            if (typeof o[k] !== "undefined") return o[k];
        }
        return undefined;
    };

    _self.handleEvents = function(){
        if(document.getElementById("startLog")){
            $("#startLog").on("click", function(e){ e.preventDefault(); _self.startLog(); });
        }
        if(document.getElementById("stopLog")){
            $("#stopLog").on("click", function(e){ e.preventDefault(); _self.stopLog(); });
        }
        if(document.getElementById("refreshLogs")){
            $("#refreshLogs").on("click", function(e){ e.preventDefault(); _self.loadLogs(); });
        }

        if(document.getElementById("prevChunk")){
            $("#prevChunk").on("click", function(e){ e.preventDefault(); _self.prevChunk(); });
        }
        if(document.getElementById("nextChunk")){
            $("#nextChunk").on("click", function(e){ e.preventDefault(); _self.nextChunk(); });
        }
        if(document.getElementById("backHome")){
            $("#backHome").on("click", function(e){ e.preventDefault(); window.location = "/"; });
        }

        $(document).on("click", "a[data-delete-log]", function(e){
            e.preventDefault();
            var name = $(this).attr("data-delete-log") || "";
            if (!name) return;
            _self.deleteLog(name);
        });

        if(document.getElementById("reloadSettings")){
            $("#reloadSettings").on("click", function(e){ e.preventDefault(); _self.settingsReload(); });
        }
        if(document.getElementById("saveSettings")){
            $("#saveSettings").on("click", function(e){ e.preventDefault(); _self.settingsSave(); });
        }

        // Default buttons support:
        // 1) <button class="defBtn" data-default-for="FIELD_ID" data-default="123">Default (123)</button>
        // 2) <button class="defBtn" data-key="CANONICAL_KEY" data-default="123">Default (123)</button>
        $(document).on("click", ".btnDefault", function(e){
            e.preventDefault();

            if (_self.uiLoading) return;

            var $b = $(this),
                key = $b.attr("data-key") || "",
                defVal = $b.attr("data-default");

            if (!key || typeof defVal === "undefined") return;

            // Update input/select if it exists
            var $field = $("#" + key);
            if ($field.length) {
                if ($field.is("select")) {
                    $field.val(String(defVal));
                } else {
                    $field.val(defVal);
                }
            }

            // Update cache so blur revert works
            _self.settingsCache[key] = String(defVal);

            // Send immediately
            _self.settingsSetNow(key, String(defVal));
        });


        if(_self.state.isSettingsPage){
            // input typing: debounced, only when value is a valid integer string
            $(document).on("input", ".settingsGrid input", function(e){
                if (_self.uiLoading) return;

                var id = $(this).attr("id") || "",
                    val = $(this).val();

                if(!id) return;

                // do not spam invalid/temporary states (prevents empty -> 0 clamp issues)
                if (!_self.isValidIntString(String(val))) return;

                _self.settingsSetDebounced(id, val);
            });

            // blur: if invalid, revert to last good value; if valid, apply now
            $(document).on("blur", ".settingsGrid input", function(e){
                if (_self.uiLoading) return;

                var id = $(this).attr("id") || "",
                    val = $(this).val();

                if(!id) return;

                if (!_self.isValidIntString(String(val))) {
                    if (typeof _self.settingsCache[id] !== "undefined") {
                        $(this).val(_self.settingsCache[id]);
                    }
                    return;
                }

                _self.settingsSetNow(id, val);
            });

            $(document).on("change", ".settingsGrid select", function(e){
                if (_self.uiLoading) return;

                var id = $(this).attr("id") || "",
                    val = $(this).val();

                if(!id) return;

                _self.settingsSetNow(id, val);
            });
        }
    };

    _self.settingsSetDebounced = function(key, value){
        var t = _self.pending.timersByKey[key];
        if (t) {
            clearTimeout(t);
            _self.pending.timersByKey[key] = null;
        }

        _self.pending.timersByKey[key] = setTimeout(function(){
            _self.pending.timersByKey[key] = null;
            _self.settingsSetNow(key, value);
        }, 250);
    };

    _self.settingsSetNow = function(key, value){
        var t = _self.pending.timersByKey[key];

        _self.pending.seq++;
        _self.pending.latestSeqByKey[key] = _self.pending.seq;

        if (t) {
            clearTimeout(t);
            _self.pending.timersByKey[key] = null;
        }

        _self.settingsSet(key, value, _self.pending.seq);
    };

    _self.settingsSet = function(key, value, seq){
        // If HTML id is a legacy key, map it for sending, but keep UI id as-is.
        var sendKey = _self.normalizeKeyForSend(key);
        var sendValue = value;

        // Normalize booleans from selects (true/false)
        if (value === true) sendValue = "true";
        if (value === false) sendValue = "false";

        var url = "/api/settings/set?key=" + encodeURIComponent(sendKey) + "&value=" + encodeURIComponent(sendValue);

        _self.setStatus("Applying " + sendKey + " = " + sendValue, false);

        _self.apiGet(url, function(code, txt){
            if (_self.pending.latestSeqByKey[key] !== seq) return;

            if(code !== 200){
                _self.setStatus("set error (" + code + ")", true);
                return;
            }

            try{
                var o = JSON.parse(txt),
                    ack = (o && o.ack) ? o.ack : "";

                if (ack) _self.setAck(ack);

                // Update cache using the UI key/id (so blur revert works)
                _self.settingsCache[key] = sendValue;

                _self.setStatus("Applied: " + sendKey + " = " + sendValue, false);
            }catch(e){
                _self.setStatus("set parse error", true);
            }
        });
    };

    _self.startLiveHud = function(){
        if (_self.liveTimer) return;
        if (!document.getElementById("hbLeftAvg") && !document.getElementById("hbRightAvg")) return;

        var tick = function(){
            _self.apiGet("/api/live", function(code, txt){
                if(code !== 200 || !txt) return;

                try{
                    var o = JSON.parse(txt);
                    if(!o || !o.ok) return;

                    if(document.getElementById("hbLeftAvg")) $("#hbLeftAvg").text(String(o.hb_left_avg));
                    if(document.getElementById("hbRightAvg")) $("#hbRightAvg").text(String(o.hb_right_avg));

                    if(document.getElementById("hbLeftAbs")) $("#hbLeftAbs").text("abs avg: " + String(o.hb_left_absavg));
                    if(document.getElementById("hbRightAbs")) $("#hbRightAbs").text("abs avg: " + String(o.hb_right_absavg));

                    // Optional: show stale data (if you add #hbAge somewhere)
                    if(document.getElementById("hbAge")) $("#hbAge").text("age: " + String(o.age_ms) + " ms");
                }catch(e){}
            });
        };

        tick();
        _self.liveTimer = setInterval(tick, 1000);
    };

    _self.renderViewerShell = function(){
        var wrap = document.querySelector(".wrap"),
            html = "";

        if(!wrap) return;

        html += "<h2>Radar Logger</h2>";

        html += "<div class=\"viewerTop\">";
        html += "<a href=\"#\" id=\"backHome\">Back</a>";
        html += "<div id=\"statusLine\" class=\"status\">Loading...</div>";
        html += "</div>";

        html += "<div class=\"viewerNav\">";
        html += "<a href=\"#\" id=\"prevChunk\">Prev 1000</a>";
        html += "<a href=\"#\" id=\"nextChunk\">Next 1000</a>";
        html += "<span id=\"chunkMeta\" class=\"meta\"></span>";
        html += "</div>";

        html += "<div class=\"gtitle\">HB100 Left</div>";
        html += "<div id=\"graphHbLeft\" class=\"dyg\" style=\"width:100%; height:220px;\"></div>";

        html += "<div class=\"gtitle\">HB100 Right</div>";
        html += "<div id=\"graphHbRight\" class=\"dyg\" style=\"width:100%; height:220px;\"></div>";

        html += "<div class=\"gtitle\">RCWL Left (0/1)</div>";
        html += "<div id=\"graphRcwlLeft\" class=\"dyg\" style=\"width:100%; height:160px;\"></div>";

        html += "<div class=\"gtitle\">RCWL Right (0/1)</div>";
        html += "<div id=\"graphRcwlRight\" class=\"dyg\" style=\"width:100%; height:160px;\"></div>";

        wrap.innerHTML = html;
    };

    _self.status = function(cb){
        _self.apiGet("/api/status", function(code, txt){
            if(code !== 200){
                _self.setStatus("status error (" + code + ")", true);
                if (cb) cb(false);
                return;
            }
            try{
                var o = JSON.parse(txt);
                _self.state.logging = o.logging ? 1 : 0;
                _self.state.file = o.file || "";
                _self.setStatus("logging=" + _self.state.logging + " file=" + _self.state.file, false);
                if (cb) cb(true, o);
            }catch(e){
                _self.setStatus("status parse error", true);
                if (cb) cb(false);
            }
        });
    };

    _self.loadLogs = function(){
        _self.apiGet("/api/list", function(code, txt){
            if(code !== 200){
                $("#logs").html("list error (" + code + ")");
                return;
            }
            try{
                var o = JSON.parse(txt),
                    html = "",
                    i = 0;

                if(o.files && o.files.length){
                    for(i=0;i<o.files.length;i++){
                        var name = o.files[i];
                        html += "<div class=\"logRow\">";
                        html += "<a class=\"logOpen\" href=\"/?view=1&name=" + encodeURIComponent(name) + "\">" + name + "</a>";
                        html += " ";
                        html += "<a class=\"logDel\" href=\"#\" data-delete-log=\"" + String(name).replace(/"/g, "&quot;") + "\">delete</a>";
                        html += "</div>";
                    }
                } else {
                    html = "no logs";
                }
                $("#logs").html(html);
            }catch(e){
                $("#logs").html("list parse error");
            }
        });
    };

    _self.startLog = function(){
        _self.apiGet("/api/start", function(code, txt){
            if(code !== 200){
                _self.setStatus("start error (" + code + ")", true);
                return;
            }
            try{
                var o = JSON.parse(txt);
                _self.state.logging = o.logging ? 1 : 0;
                _self.state.file = o.file || "";
                if (_self.state.logging) _self.setStatus("Logging started: " + _self.state.file, false);
                else _self.setStatus("Start requested, but logging is still OFF", true);
            }catch(e){
                _self.setStatus("start parse error", true);
            }
            _self.loadLogs();
        });
    };

    _self.stopLog = function(){
        _self.apiGet("/api/stop", function(code, txt){
            if(code !== 200){
                _self.setStatus("stop error (" + code + ")", true);
                return;
            }
            try{
                var o = JSON.parse(txt);
                _self.state.logging = o.logging ? 1 : 0;
                _self.state.file = o.file || "";
                if (!_self.state.logging) _self.setStatus("Logging stopped", false);
                else _self.setStatus("Stop requested, but logging is still ON (" + _self.state.file + ")", true);
            }catch(e){
                _self.setStatus("stop parse error", true);
            }
            _self.loadLogs();
        });
    };

    _self.deleteLog = function(name){
        var url = "/api/delete?name=" + encodeURIComponent(name);
        _self.apiGet(url, function(code, txt){
            if(code !== 200){
                _self.setStatus("delete error (" + code + ")", true);
                return;
            }
            _self.setStatus("Deleted: " + name, false);
            _self.loadLogs();
        });
    };

    _self.prevChunk = function(){
        if(_self.state.offset <= 0) return;
        _self.state.offset -= _self.state.limit;
        if(_self.state.offset < 0) _self.state.offset = 0;
        _self.loadChunk();
    };

    _self.nextChunk = function(){
        _self.state.offset += _self.state.limit;
        _self.loadChunk();
    };

    _self.makeUnderlay = function(side){
        return function(ctx, area, g){
            var spans = (side === "left") ? _self.shading.left : _self.shading.right,
                i = 0;

            if (!spans || !spans.length) return;

            ctx.save();
            ctx.fillStyle = "rgba(255, 235, 59, 0.22)";

            for(i=0;i<spans.length;i++){
                var a = spans[i][0],
                    b = spans[i][1],
                    x1 = g.toDomXCoord(a),
                    x2 = g.toDomXCoord(b);

                if (x2 < area.x) continue;
                if (x1 > area.x + area.w) continue;

                if (x1 < area.x) x1 = area.x;
                if (x2 > area.x + area.w) x2 = area.x + area.w;

                ctx.fillRect(x1, area.y, (x2 - x1), area.h);
            }

            ctx.restore();
        };
    };

    _self.ensureDygraph = function(key, elId, labels, color, yOpt, shadeSide){
        var el = document.getElementById(elId),
            opts = null;

        if (!el) return;
        if (_self.graphs[key]) return;

        opts = {
            labels: labels,
            colors: [color],
            legend: "always",
            labelsSeparateLines: true,
            strokeWidth: 2,
            drawPoints: false,
            highlightCircleSize: 3,
            connectSeparatedPoints: false,
            animatedZooms: true,
            xlabel: "t (s)",
            showRoller: false,
            rollPeriod: 1,
            showRangeSelector: false,
            fillGraph: false,
            drawGrid: true,
            drawAxesAtZero: false,
            axisLabelFontSize: 12,
            labelsDivStyles: { "textAlign": "right" },
            labelsDivWidth: 260,
            yLabelWidth: 50
        };

        if (shadeSide) {
            opts.underlayCallback = _self.makeUnderlay(shadeSide);
        }

        if (yOpt && typeof yOpt === "object") {
            if (typeof yOpt.valueRangeMin !== "undefined" && typeof yOpt.valueRangeMax !== "undefined") {
                opts.valueRange = [yOpt.valueRangeMin, yOpt.valueRangeMax];
            }
            if (yOpt.ylabel) opts.ylabel = yOpt.ylabel;
        }

        _self.graphs[key] = new Dygraph(el, [[0, 0]], opts);
    };

    _self.setDyData = function(key, rows){
        if (!_self.graphs[key]) return;
        _self.graphs[key].updateOptions({ file: rows });
    };

    _self.computeSpans = function(tArr, flagArr){
        var spans = [],
            i = 0,
            inOn = 0,
            startT = 0;

        if (!tArr || !flagArr) return spans;
        if (tArr.length !== flagArr.length) return spans;
        if (tArr.length < 2) return spans;

        for(i=0;i<tArr.length;i++){
            var on = flagArr[i] ? 1 : 0,
                t = tArr[i];

            if (!inOn && on) {
                inOn = 1;
                startT = t;
            } else if (inOn && !on) {
                inOn = 0;
                spans.push([startT, t]);
            }
        }

        if (inOn) {
            spans.push([startT, tArr[tArr.length - 1]]);
        }

        return spans;
    };

    _self.parseChunk = function(txt){
        var lines = txt.split("\n"),
            hbL = [],
            hbR = [],
            rcL = [],
            rcR = [],
            tL = [],
            tR = [],
            ledLFlags = [],
            ledRFlags = [],
            count = 0,
            i = 0,
            lastTs = -1;

        for(i=0;i<lines.length;i++){
            var line = $.trim(lines[i]),
                p = null,
                ints = [],
                j = 0,
                v = 0,
                l = 0, r = 0, rl = 0, rr = 0, ledL = 0, ledR = 0,
                tsMs = 0,
                t = 0;

            if(!line) continue;

            p = line.split(",");
            if(!p || p.length < 5) continue;

            // parse all tokens to ints (tolerate extra fields at the end)
            for(j=0;j<p.length;j++){
                v = parseInt($.trim(p[j]), 10);
                if (!isNaN(v)) ints.push(v);
            }

            if (ints.length < 5) continue;

            l = ints[0];
            r = ints[1];
            rl = ints[2];
            rr = ints[3];

            // timestamp = last int (more tolerant)
            tsMs = ints[ints.length - 1];

            // if standard 7-field format exists, ledL/ledR are 5th/6th
            if (ints.length >= 7) {
                ledL = ints[4];
                ledR = ints[5];
            } else {
                ledL = 0;
                ledR = 0;
            }

            if (tsMs <= lastTs) continue;
            lastTs = tsMs;

            t = tsMs / 1000.0;

            hbL.push([t, l]);
            hbR.push([t, r]);
            rcL.push([t, (rl ? 1 : 0)]);
            rcR.push([t, (rr ? 1 : 0)]);

            tL.push(t);
            tR.push(t);

            ledLFlags.push(ledL ? 1 : 0);
            ledRFlags.push(ledR ? 1 : 0);

            count++;
        }

        _self.shading.left = _self.computeSpans(tL, ledLFlags);
        _self.shading.right = _self.computeSpans(tR, ledRFlags);

        return { count: count, hbL: hbL, hbR: hbR, rcL: rcL, rcR: rcR };
    };

    _self.loadChunk = function(){
        var url = "/api/chunk?name=" + encodeURIComponent(_self.state.logName) +
            "&offset=" + _self.state.offset +
            "&limit=" + _self.state.limit;

        _self.apiGet(url, function(code, txt){
            var parsed = null;

            if(code !== 200){
                _self.setStatus("chunk error (" + code + ")", true);
                return;
            }

            parsed = _self.parseChunk(txt);

            _self.state.lastCount = parsed.count;
            _self.showNav();

            _self.setStatus("file=" + _self.state.logName + " offset=" + _self.state.offset + " count=" + _self.state.lastCount, false);

            _self.ensureDygraph("hbLeft", "graphHbLeft", ["t", "HB100 Left"], _self.colors.hbLeft, { ylabel: "val" }, "left");
            _self.ensureDygraph("hbRight", "graphHbRight", ["t", "HB100 Right"], _self.colors.hbRight, { ylabel: "val" }, "right");

            _self.ensureDygraph("rcwlLeft", "graphRcwlLeft", ["t", "RCWL Left"], _self.colors.rcwlLeft, { ylabel: "0/1", valueRangeMin: -0.1, valueRangeMax: 1.1 }, "left");
            _self.ensureDygraph("rcwlRight", "graphRcwlRight", ["t", "RCWL Right"], _self.colors.rcwlRight, { ylabel: "0/1", valueRangeMin: -0.1, valueRangeMax: 1.1 }, "right");

            _self.setDyData("hbLeft", parsed.hbL);
            _self.setDyData("hbRight", parsed.hbR);
            _self.setDyData("rcwlLeft", parsed.rcL);
            _self.setDyData("rcwlRight", parsed.rcR);

            try {
                if (_self.graphs.hbLeft) _self.graphs.hbLeft.resize();
                if (_self.graphs.hbRight) _self.graphs.hbRight.resize();
                if (_self.graphs.rcwlLeft) _self.graphs.rcwlLeft.resize();
                if (_self.graphs.rcwlRight) _self.graphs.rcwlRight.resize();
            } catch(e) {}
        });
    };

    _self.settingsPopulate = function(o){
        if(!o) return;

        _self.uiLoading = 1;

        // Store canonical values in cache for blur revert; also fill UI if elements exist.

        // Timing / periods
        var sp = _self.pickFirst(o, ["SAMPLE_PERIOD_MICROSECONDS", "SAMPLE_PERIOD_US"]);
        if (typeof sp !== "undefined") { _self.settingsCache["SAMPLE_PERIOD_MICROSECONDS"] = sp; _self.setFieldValueIfExists("SAMPLE_PERIOD_MICROSECONDS", sp); }

        var mn = _self.pickFirst(o, ["MIN_PERIOD_MICROSECONDS", "MIN_PERIOD_US"]);
        if (typeof mn !== "undefined") { _self.settingsCache["MIN_PERIOD_MICROSECONDS"] = mn; _self.setFieldValueIfExists("MIN_PERIOD_MICROSECONDS", mn); }

        var mx = _self.pickFirst(o, ["MAX_PERIOD_MICROSECONDS", "MAX_PERIOD_US"]);
        if (typeof mx !== "undefined") { _self.settingsCache["MAX_PERIOD_MICROSECONDS"] = mx; _self.setFieldValueIfExists("MAX_PERIOD_MICROSECONDS", mx); }

        // Motion hold
        var mh = _self.pickFirst(o, ["MOTION_HOLD_MILISECONDS", "MOTION_HOLD_MS"]);
        if (typeof mh !== "undefined") { _self.settingsCache["MOTION_HOLD_MILISECONDS"] = mh; _self.setFieldValueIfExists("MOTION_HOLD_MILISECONDS", mh); }

        // Events
        if (typeof o.EVENTS_TO_TRIGGER !== "undefined") {
            _self.settingsCache["EVENTS_TO_TRIGGER"] = o.EVENTS_TO_TRIGGER;
            _self.setFieldValueIfExists("EVENTS_TO_TRIGGER", o.EVENTS_TO_TRIGGER);
        }

        // RCWL
        var rc = _self.pickFirst(o, ["RCWL_MIN_ACTIVE_MILISECONDS", "RCWL_MIN_ACTIVE_MS"]);
        if (typeof rc !== "undefined") { _self.settingsCache["RCWL_MIN_ACTIVE_MILISECONDS"] = rc; _self.setFieldValueIfExists("RCWL_MIN_ACTIVE_MILISECONDS", rc); }

        if (typeof o.ENABLE_RCWL_LEFT !== "undefined") { _self.settingsCache["ENABLE_RCWL_LEFT"] = o.ENABLE_RCWL_LEFT ? "true" : "false"; _self.setSelectBoolIfExists("ENABLE_RCWL_LEFT", !!o.ENABLE_RCWL_LEFT); }
        if (typeof o.ENABLE_RCWL_RIGHT !== "undefined") { _self.settingsCache["ENABLE_RCWL_RIGHT"] = o.ENABLE_RCWL_RIGHT ? "true" : "false"; _self.setSelectBoolIfExists("ENABLE_RCWL_RIGHT", !!o.ENABLE_RCWL_RIGHT); }

        // Threshold
        if (typeof o.MIN_AMPLITUDE !== "undefined") {
            _self.settingsCache["MIN_AMPLITUDE"] = o.MIN_AMPLITUDE;
            _self.setFieldValueIfExists("MIN_AMPLITUDE", o.MIN_AMPLITUDE);
        }

        if (typeof o.MIN_AMPLITUDE_LEFT !== "undefined") {
            _self.settingsCache["MIN_AMPLITUDE_LEFT"] = o.MIN_AMPLITUDE_LEFT;
            _self.setFieldValueIfExists("MIN_AMPLITUDE_LEFT", o.MIN_AMPLITUDE_LEFT);
        }

        if (typeof o.MIN_AMPLITUDE_RIGHT !== "undefined") {
            _self.settingsCache["MIN_AMPLITUDE_RIGHT"] = o.MIN_AMPLITUDE_RIGHT;
            _self.setFieldValueIfExists("MIN_AMPLITUDE_RIGHT", o.MIN_AMPLITUDE_RIGHT);
        }

        if (typeof o.NOISE_MULT_LEFT !== "undefined") {
            _self.settingsCache["NOISE_MULT_LEFT"] = o.NOISE_MULT_LEFT;
            _self.setFieldValueIfExists("NOISE_MULT_LEFT", o.NOISE_MULT_LEFT);
        }

        if (typeof o.NOISE_MULT_RIGHT !== "undefined") {
            _self.settingsCache["NOISE_MULT_RIGHT"] = o.NOISE_MULT_RIGHT;
            _self.setFieldValueIfExists("NOISE_MULT_RIGHT", o.NOISE_MULT_RIGHT);
        }

        if (typeof o.NOISE_OFFSET_LEFT !== "undefined") {
            _self.settingsCache["NOISE_OFFSET_LEFT"] = o.NOISE_OFFSET_LEFT;
            _self.setFieldValueIfExists("NOISE_OFFSET_LEFT", o.NOISE_OFFSET_LEFT);
        }

        if (typeof o.NOISE_OFFSET_RIGHT !== "undefined") {
            _self.settingsCache["NOISE_OFFSET_RIGHT"] = o.NOISE_OFFSET_RIGHT;
            _self.setFieldValueIfExists("NOISE_OFFSET_RIGHT", o.NOISE_OFFSET_RIGHT);
        }

        var nu = _self.pickFirst(o, ["NOISE_AVERAGE_UPDATE_SPEED", "NOISE_ALPHA_SHIFT"]);
        if (typeof nu !== "undefined") {
            _self.settingsCache["NOISE_AVERAGE_UPDATE_SPEED"] = nu;
            _self.setFieldValueIfExists("NOISE_AVERAGE_UPDATE_SPEED", nu);

            // if old field exists in HTML, populate it too
            _self.settingsCache["NOISE_ALPHA_SHIFT"] = nu;
            _self.setFieldValueIfExists("NOISE_ALPHA_SHIFT", nu);
        }

        _self.uiLoading = 0;
    };

    _self.settingsLoad = function(){
        _self.setStatus("Loading settings...", false);
        _self.apiGet("/api/settings/get", function(code, txt){
            if(code !== 200){
                _self.setStatus("settings get error (" + code + ")", true);
                return;
            }
            try{
                var o = JSON.parse(txt);
                _self.settingsPopulate(o);
                _self.setAck("none");
                _self.setStatus("Settings loaded", false);
            }catch(e){
                _self.setStatus("settings parse error", true);
            }
        });
    };

    _self.settingsReload = function(){
        _self.setStatus("Reloading from SD and applying...", false);
        _self.apiGet("/api/settings/reload", function(code, txt){
            if(code !== 200){
                _self.setStatus("reload error (" + code + ")", true);
                return;
            }
            _self.settingsLoad();
            _self.setStatus("Reloaded and applied", false);
        });
    };

    _self.settingsSave = function(){
        _self.setStatus("Saving to SD...", false);
        _self.apiGet("/api/settings/save", function(code, txt){
            if(code !== 200){
                _self.setStatus("save error (" + code + ")", true);
                return;
            }
            _self.setStatus("Saved to SD", false);
        });
    };

    _self.init = function(){
        _self.state.isSettingsPage = document.getElementById("settingsPage") ? 1 : 0;

        _self.state.viewMode = (_self.qs("view") === "1") ? 1 : 0;
        _self.state.logName = _self.qs("name");
        _self.state.offset = 0;
        _self.state.limit = 1000;
        _self.state.lastCount = 0;

        if(_self.state.viewMode && _self.state.logName){
            _self.renderViewerShell();
        }

        _self.handleEvents();

        if(_self.state.isSettingsPage){
            _self.settingsLoad();
            _self.startLiveHud(); // shows HB100 1s averages if the HTML has the elements
            return;
        }

        if(_self.state.viewMode && _self.state.logName){
            _self.loadChunk();
        } else {
            _self.status();
            _self.loadLogs();
        }
    };
};

$(window).load(function(){
    window.RadarLogger = new RadarLogger();
    window.RadarLogger.init();
});
