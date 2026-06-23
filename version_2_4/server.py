import asyncio
import os
from fastapi import FastAPI, WebSocket
import azure.cognitiveservices.speech as speechsdk
from dotenv import load_dotenv
from openai import AzureOpenAI
from azure.search.documents import SearchClient
from azure.search.documents.models import VectorizedQuery
from azure.core.credentials import AzureKeyCredential
from contextlib import asynccontextmanager
import azure.cognitiveservices.speech as speechsdk
import asyncio

# ================= CONFIG =================
load_dotenv('.env.data')

SPEECH_KEY = os.getenv("AZURE_SPEECH_KEY")
SPEECH_REGION = os.getenv("AZURE_SPEECH_REGION")
AZURE_OPENAI_KEY = os.getenv("AZURE_OPENAI_KEY")
AZURE_OPENAI_ENDPOINT = os.getenv("AZURE_OPENAI_ENDPOINT")
AZURE_OPENAI_API_VERSION = os.getenv("AZURE_OPENAI_API_VERSION")
SEARCH_ENDPOINT = os.getenv("AZURE_SEARCH_ENDPOINT")
SEARCH_KEY = os.getenv("AZURE_SEARCH_KEY")
INDEX_NAME = os.getenv("AZURE_SEARCH_INDEX")

# ================= CLIENTS =================
client = AzureOpenAI(
    api_key=AZURE_OPENAI_KEY,
    api_version=AZURE_OPENAI_API_VERSION,
    azure_endpoint=AZURE_OPENAI_ENDPOINT
)

search_client = SearchClient(
    endpoint=SEARCH_ENDPOINT,
    index_name=INDEX_NAME,
    credential=AzureKeyCredential(SEARCH_KEY)
)

# ================= HOUSE STATE ==============
house_state = {
    "ac_power": False,
    "ac_temp": 24,
    "light": False
}

# ================= LIFESPAN =================
@asynccontextmanager
async def lifespan(app: FastAPI):
    print("🔥 Warming up...")
    try:
        client.chat.completions.create(
            model="gpt-4o-mini",
            messages=[{"role": "user", "content": "hi"}],
            max_tokens=5
        )
        speech_config = speechsdk.SpeechConfig(subscription=SPEECH_KEY, region=SPEECH_REGION)
        synthesizer = speechsdk.SpeechSynthesizer(speech_config=speech_config, audio_config=None)
        synthesizer.speak_text_async("ok").get()
        print("✅ Warmup done")
        yield
    except Exception as e:
        print(f"Warmup error: {e}")
        raise
    finally:
        print("Shutting down app...")

app = FastAPI(lifespan=lifespan)

# ================= HELPERS =================
def normalize_text(text: str) -> str:
    text = text.lower()
    for k, v in {"ovum": "oven", 
                 "tekka": "teka", 
                 "dekha": "teka", 
                 "cup tok": "cooktop", 
                 "cup top": "cooktop",  
                 "pen": "pan", 
                 "pant": "pan", 
                 "tika": "teka"
                 }.items():
        text = text.replace(k, v)
    return text

def is_valid_utterance(text: str) -> bool:
    clean = text.strip(" .,?!*-_")
    if len(clean) < 3:
        return False
    if all(c in "*-_. " for c in clean):
        return False
    return True

def fast_intent(text: str):

    text = text.lower()

    # --------------------
    # LIGHTS
    # --------------------
    LIGHT_ON_PHRASES = [
        "turn on the light",
        "it's too dark",
        "too dark",
        "more light",
        "light please",
        "brighten the room",
        "make it brighter",
        "i can't see",
        "feeling dark",
        "light on please"
    ]

    LIGHT_OFF_PHRASES = [
        "turn off the light",
        "dim the light",
        "too bright",
        "less light",
        "light off please",
        "make it darker",
        "darken the room"
    ]

    if any(x in text for x in LIGHT_ON_PHRASES):
        return {"intent": "LIGHT_ON"}

    if (
        "light" in text
        and ("turn on" in text or "start" in text)
    ):
        return {
            "intent": "LIGHT_ON"
        }

    if any(x in text for x in LIGHT_OFF_PHRASES):
        return {"intent": "LIGHT_OFF"}

    if (
        "light" in text
        and ("turn off" in text or "stop" in text)
    ):
        return {
            "intent": "LIGHT_OFF"
        }

    if ("is the light on" in text or "light status" in text):
        return {"intent":"LIGHT_STATUS"}
    # --------------------
    # AC
    # --------------------
    HOT_PHRASES = [
        "i feel hot",
        "too hot",
        "it's hot",
        "very hot",
        "warm here",
        "cool the room",
        "cooler please",
        "I'm hot",
        "feeling hot",
        "make it cooler",
        "lower the temperature"
    ]

    COLD_PHRASES = [
        "i feel cold",
        "too cold",
        "it's cold",
        "warm it up",
        "increase temperature",
        "heat up the room",
        "make it warmer",
        "feeling cold",
        "too chilly",
        "it's freezing",
        "I'm cold",
        "feeling cold",
        "raise the temperature"
    ]

    if any(x in text for x in HOT_PHRASES):
        return {
            "intent":"SET_AC",
            "temperature":22
        }

    if any(x in text for x in COLD_PHRASES):
        return {
            "intent": "SET_AC",
            "temperature": 25
        }

    if (
        "air conditioner" in text
        and ("turn on" in text or "start" in text)
    ):
        return {
            "intent": "SET_AC",
            "temperature": house_state["ac_temp"]
        }

    if (
        "air conditioner" in text
        and ("turn off" in text or "stop" in text)
    ):
        return {
            "intent": "AC_OFF"
        }

    if ("what temperature" in text or "ac status" in text or "air conditioner status" in text):
        return {"intent":"AC_STATUS"}
    # --------------------
    # OLD DEMOS
    # --------------------

    if text == "turn on":
        return "Turning on the cooktop"

    if text == "turn off":
        return "Turning off the cooktop"

    if "power" in text:
        return "Adjusting power level"

    return None

def needs_rag(text: str) -> bool:
    keywords = [
        "error", "manual", "maintenance","installation", "guide", "troubleshoot", 
        "troubleshooting", "problem", "issue", "code", "fault"
    ]
    return any(k in text.lower() for k in keywords)

def get_embedding(text: str):
    response = client.embeddings.create(model="text-embedding-3-large", input=text)
    return response.data[0].embedding

def search_docs(query: str, k: int = 3):
    print("🔍 Searching RAG")
    embedding = get_embedding(query)
    print("🔍 Embedding done")
    results = search_client.search(
        search_text=None,
        vector_queries=[VectorizedQuery(vector=embedding, k_nearest_neighbors=k, fields="embedding")],
        select=["content"]
    )
    print("🔍 Search done")
    return "\n".join(r["content"] for r in results)

async def execute_action(intent, ws):
    import random
    ### AC ON
    if intent["intent"] == "SET_AC":
        temp = intent["temperature"]
        house_state["ac_power"] = True
        house_state["ac_temp"] = temp
        print(f"🏠 AC -> {temp}°C")
        return random.choice([
            f"I'll make the room more comfortable. Air conditioner set to {temp} degrees.",
            f"Sure. Cooling the room to {temp} degrees.",
            f"Done. Air conditioner now set to {temp}."
        ])
    ### AC OFF
    if intent["intent"] == "AC_OFF":
        house_state["ac_power"] = False
        print("🏠 AC OFF")
        return random.choice([
            "Air conditioner switched off.",
            "Okay. Turning off the air conditioner.",
            "Done. Cooling stopped."
        ])
    ### AC STATUS
    if intent["intent"] == "AC_STATUS":
        if house_state["ac_power"]:
            return f"The air conditioner is on at {house_state['ac_temp']} degrees."
        return "The air conditioner is off."
    ### LIGHT ON
    if intent["intent"] == "LIGHT_ON":
        house_state["light"] = True
        await ws.send_text("LIGHT_ON")
        return random.choice([
            "Turning on the light.",
            "The room should be brighter now.",
            "Sure. Light switched on."
        ])
    ### LIGHT OFF
    if intent["intent"] == "LIGHT_OFF":
        house_state["light"] = False
        await ws.send_text("LIGHT_OFF")
        return random.choice([
            "Turning off the light.",
            "Dimming the room.",
            "Sure. Light switched off."
        ])
    ### LIGHT STATUS
    if intent["intent"] == "LIGHT_STATUS":
        if house_state["light"]:
            return "The light is currently on."
        return "The light is off."
    return "Done."
# ================= LLM =================
MAX_HISTORY = 4

async def process_with_rag(text: str, conversation: list, ws: WebSocket) -> str:
    intent = fast_intent(text)
    if intent:
        return await execute_action(intent, ws)

    if needs_rag(text):
        context = await asyncio.to_thread(search_docs, text)
        prompt = f"""You are a house assistant.
Use FIRST the following manual context to answer.
If answer is not in the manual, use your general knowledge.
Manual:
{context}
Question:
{text}"""
    else:
        prompt = text

    messages = [{
        "role": "system",
        "content": (
            "You are a smart house assistant. "
            "Answer in English. Use manual data when relevant, otherwise use general knowledge. "
            "Always include appliance usage (temperature, power). "
            "Be step-by-step but concise, short sentences, max 4 steps. "
            "Do not use characters like #, -, 1., 2. — just new lines for steps. "
            "Answers as short as possible, ideally under 20 words."
        )
    }]

    for m in conversation:
        messages.append(m)
    messages.append({"role": "user", "content": prompt})

    response = await asyncio.to_thread(
        client.chat.completions.create,
        model="gpt-4o-mini",
        messages=messages,
        max_tokens=100,
        temperature=0.3
    )

    answer = response.choices[0].message.content

    conversation.append({"role": "user", "content": text})
    conversation.append({"role": "assistant", "content": answer})
    if len(conversation) > MAX_HISTORY * 2:
        del conversation[:-MAX_HISTORY * 2]

    return answer

# ================= TTS =================

async def tts_stream(text: str):
    speech_config = speechsdk.SpeechConfig(
        subscription=SPEECH_KEY,
        region=SPEECH_REGION
    )

    speech_config.set_speech_synthesis_output_format(
        speechsdk.SpeechSynthesisOutputFormat.Raw16Khz16BitMonoPcm
    )

    synthesizer = speechsdk.SpeechSynthesizer(
        speech_config=speech_config,
        audio_config=None
    )

    # Start synthesis
    result = await asyncio.to_thread(
        synthesizer.speak_text_async(text).get
    )

    if result.reason != speechsdk.ResultReason.SynthesizingAudioCompleted:
        print("❌ TTS failed")
        return

    # Create stream reader
    stream = speechsdk.AudioDataStream(result)

    buffer = bytes(1024)  # IMPORTANT: immutable bytes, NOT bytearray

    while True:
        size = stream.read_data(buffer)

        if size == 0:
            break

        yield buffer[:size]

        await asyncio.sleep(0)

# ================= WEBSOCKET =================
@app.websocket("/ws/audio")
async def websocket_audio(ws: WebSocket):
    await ws.accept()
    print("✅ WebSocket accepted")

    is_activated = False
    conversation = []

    speech_config = speechsdk.SpeechConfig(subscription=SPEECH_KEY, region=SPEECH_REGION)
    speech_config.speech_recognition_language = "en-US"

    audio_format = speechsdk.audio.AudioStreamFormat(
        samples_per_second=16000,
        bits_per_sample=16,
        channels=1
    )
    stream = speechsdk.audio.PushAudioInputStream(
        stream_format=audio_format
    )
    audio_config = speechsdk.audio.AudioConfig(stream=stream)
    recognizer = speechsdk.SpeechRecognizer(speech_config=speech_config, audio_config=audio_config)
    print("✅ Recognizer created")

    text_queue = asyncio.Queue()
    control_queue = asyncio.Queue()   # ← control barge-in
    loop = asyncio.get_event_loop()   # ← captured once in async context

    WAKE_WORD = "assistant"
    SESSION_TIMEOUT = 45
    FILLER_WORDS = {"hello", "hi", "hey", "ok", "okay", "yes", "no"}

    last_activity = loop.time()

    stop_requested = False

    STOP_WORDS = {"stop", "cancel", "enough", "shut up", "quiet", "silence"}

    ws_lock = asyncio.Lock()

    def on_recognized(evt):
        nonlocal is_activated, last_activity, stop_requested
        try:
            last_activity = loop.time()
            text = evt.result.text.strip()
            print(f"🎯 Heard: '{text}'")

            if not text or not is_valid_utterance(text):
                return

            # Stop command works whether activated or not
            text_clean = text.lower().strip(" .,?!")
            if any(word in text_clean for word in STOP_WORDS):
                print("🛑 Stop requested")
                stop_requested = True
                loop.call_soon_threadsafe(control_queue.put_nowait, "STOP_AUDIO")
                return

            if not is_activated:
                if WAKE_WORD in text.lower():
                    is_activated = True
                    stop_requested = False   # ← clear on wake
                    print("✅ Wake word detected!")
                    clean = text.lower()
                    for phrase in ["hello assistant", "hey assistant", "assistant"]:
                        clean = clean.replace(phrase, "")
                    clean = clean.strip(" ,.?!")
                    loop.call_soon_threadsafe(text_queue.put_nowait, clean if clean else "__wake__")
            else:
                if text.lower().strip(" .,?!") in FILLER_WORDS:
                    print(f"⏭️ Skipping filler: '{text}'")
                    return
                print(f"💬 Queuing: '{text}'")
                loop.call_soon_threadsafe(text_queue.put_nowait, text)

        except Exception as e:
            import traceback
            print(f"💥 CRASH in on_recognized: {e}")
            traceback.print_exc()

    def on_canceled(evt):
        print(f"❌ CANCELED: reason={evt.reason} details={evt.cancellation_details.error_details}")

    def on_session_started(evt):
        print("🟢 Recognition session started")

    def on_session_stopped(evt):
        print("🔴 Recognition session stopped")

    recognizer.recognized.connect(on_recognized)
    recognizer.canceled.connect(on_canceled)
    recognizer.session_started.connect(on_session_started)
    recognizer.session_stopped.connect(on_session_stopped)
    recognizer.start_continuous_recognition()
    print("✅ Recognition started")

    async def receive_audio():
        count = 0
        try:
            while True:
                data = await ws.receive_bytes()
                stream.write(data)
                if count % 50 == 0:
                    print(
                        f"chunk={count} bytes={len(data)}"
                    )
                count += 1
                if count % 50 == 0:
                    print(f"📦 Chunks received: {count}")
        except Exception as e:
            print(f"⚠️ receive_audio closed: {e}")
            stream.close()
            recognizer.stop_continuous_recognition()
    
    async def process():
        nonlocal last_activity, stop_requested

        while True:
            try:
                text = normalize_text(await text_queue.get())
                print(f"📨 Processing: {text}")

                stop_requested = False

                if text == "__wake__":
                    response_text = "Hello! How can I help you?"
                else:
                    try:
                        response_text = await asyncio.wait_for(
                            process_with_rag(text, conversation, ws),
                            timeout=20
                        )
                    except asyncio.TimeoutError:
                        print("❌ LLM timeout")
                        response_text = "Sorry, I took too long to respond."


                print(f"🤖 Assistant: {response_text}")

                last_activity = loop.time()

                aborted = False
                started = False

                async for chunk in tts_stream(response_text):

                    if not started:
                        print("🔊 TTS streaming started")
                        started = True

                    if stop_requested:
                        print("🛑 TTS aborted")
                        aborted = True
                        break

                    async with ws_lock:
                        await ws.send_bytes(chunk)

                if not aborted:
                    async with ws_lock:
                        await ws.send_text("END_AUDIO")

            except Exception as e:
                import traceback
                print(f"⚠️ process error: {e}")
                traceback.print_exc()
                try:
                    async for chunk in tts_stream("Sorry, I had a problem with that."):
                        await ws.send_bytes(chunk)
                except:
                    pass

    async def session_watcher():
        nonlocal is_activated
        while True:
            await asyncio.sleep(1)
            if is_activated and loop.time() - last_activity > SESSION_TIMEOUT:
                is_activated = False
                print("💤 Session timeout — back to sleep.")
                try:
                    async with ws_lock:
                        await ws.send_text("SLEEP")
                except Exception as e:
                    print(f"⚠️ session_watcher error: {e}")

    async def control_sender():
        while True:
            msg = await control_queue.get()
            try:
                await ws.send_text(msg)
            except Exception as e:
                print(e)

    await asyncio.gather(receive_audio(), process(), session_watcher(), control_sender())
