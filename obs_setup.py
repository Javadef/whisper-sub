import asyncio
import base64
import hashlib
import json
import uuid
import websockets

PASSWORD = "Qf1U9KWl0NeYZZ3l"
URI = "ws://localhost:4455"


def make_auth(password, salt, challenge):
    secret = base64.b64encode(
        hashlib.sha256((password + salt).encode()).digest()
    ).decode()
    auth = base64.b64encode(
        hashlib.sha256((secret + challenge).encode()).digest()
    ).decode()
    return auth


async def req(ws, request_type, request_data=None):
    rid = str(uuid.uuid4())
    msg = {
        "op": 6,
        "d": {
            "requestType": request_type,
            "requestId": rid,
            "requestData": request_data or {},
        },
    }
    await ws.send(json.dumps(msg))
    while True:
        raw = await ws.recv()
        data = json.loads(raw)
        if data["op"] == 7 and data["d"]["requestId"] == rid:
            if not data["d"]["requestStatus"]["result"]:
                code = data["d"]["requestStatus"].get("code")
                comment = data["d"]["requestStatus"].get("comment", "")
                print(f"  [warn] {request_type} failed code={code} {comment}")
                return None
            return data["d"].get("responseData", {})


async def main():
    async with websockets.connect(URI) as ws:
        # Hello
        hello = json.loads(await ws.recv())
        assert hello["op"] == 0
        auth_info = hello["d"].get("authentication")

        # Identify
        identify = {"op": 1, "d": {"rpcVersion": 1, "eventSubscriptions": 0}}
        if auth_info:
            identify["d"]["authentication"] = make_auth(
                PASSWORD, auth_info["salt"], auth_info["challenge"]
            )
        await ws.send(json.dumps(identify))

        identified = json.loads(await ws.recv())
        assert identified["op"] == 2, f"Expected Identified(2), got {identified}"
        print("[obs] connected and identified")

        # Set video settings: 1920x1080 @ 29.97
        await req(ws, "SetVideoSettings", {
            "fpsNumerator": 30000,
            "fpsDenominator": 1001,
            "baseWidth": 1920,
            "baseHeight": 1080,
            "outputWidth": 1920,
            "outputHeight": 1080,
        })
        print("[obs] video set: 1920x1080 @ 29.97fps")

        # Get current scene
        scene_resp = await req(ws, "GetCurrentProgramScene")
        scene_name = scene_resp["currentProgramSceneName"]
        print(f"[obs] scene: {scene_name}")

        # Check existing inputs
        inputs_resp = await req(ws, "GetInputList")
        existing = [i["inputName"] for i in inputs_resp.get("inputs", [])]
        if "WhisperSub" in existing:
            print("[obs] source 'WhisperSub' already exists, skipping")
        else:
            await req(ws, "CreateInput", {
                "sceneName": scene_name,
                "inputName": "WhisperSub",
                "inputKind": "browser_source",
                "inputSettings": {
                    "url": "http://localhost:8080",
                    "width": 1920,
                    "height": 1080,
                    "shutdown": False,
                    "reroute_audio": False,
                },
                "sceneItemEnabled": True,
            })
            print("[obs] browser source 'WhisperSub' created → http://localhost:8080")

        # List outputs
        outputs_resp = await req(ws, "GetOutputList")
        outputs = outputs_resp.get("outputs", [])
        print("\n[obs] outputs:")
        for o in outputs:
            print(f"  {o['outputName']}  active={o['outputActive']}")

        # Try start AJA output
        aja = next((o for o in outputs if "aja" in o["outputName"].lower()), None)
        if aja:
            if not aja["outputActive"]:
                await req(ws, "StartOutput", {"outputName": aja["outputName"]})
                print(f"[obs] started: {aja['outputName']}")
            else:
                print(f"[obs] already active: {aja['outputName']}")
        else:
            print("[obs] no AJA output found — start manually: Tools → AJA I/O Device Output → Start")


asyncio.run(main())
