import tkinter as tk
from tkinter import filedialog, messagebox
import threading
import time
import opuslib
import struct
import numpy as np
import sounddevice as sd
import os


def play_p3_file(input_file, stop_event=None, pause_event=None):
    """
    Play P3 format audio file
    P3 format: [1 byte type, 1 byte reserved, 2 bytes length, Opus data]
    """
    # Initialize Opus decoder
    sample_rate = 16000  # Sample rate fixed at 16000Hz
    channels = 1  # Mono channel
    decoder = opuslib.Decoder(sample_rate, channels)
    
    # Frame size (60ms)
    frame_size = int(sample_rate * 60 / 1000)
    
    # Open audio stream
    stream = sd.OutputStream(
        samplerate=sample_rate,
        channels=channels,
        dtype='int16'
    )
    stream.start()
    
    try:
        with open(input_file, 'rb') as f:
            print(f"Now playing: {input_file}")
            
            while True:
                if stop_event and stop_event.is_set():
                    break

                if pause_event and pause_event.is_set():
                    time.sleep(0.1)
                    continue

                # Read header (4 bytes)
                header = f.read(4)
                if not header or len(header) < 4:
                    break
                
                # Parse header
                packet_type, reserved, data_len = struct.unpack('>BBH', header)
                
                # Read Opus data
                opus_data = f.read(data_len)
                if not opus_data or len(opus_data) < data_len:
                    break
                
                # Decode Opus data
                pcm_data = decoder.decode(opus_data, frame_size)
                
                # Convert bytes to numpy array
                audio_array = np.frombuffer(pcm_data, dtype=np.int16)
                
                # Play audio
                stream.write(audio_array)
                
    except KeyboardInterrupt:
        print("\nPlayback stopped")
    finally:
        stream.stop()
        stream.close()
        print("Playback completed")


class P3PlayerApp:
    def __init__(self, root):
        self.root = root
        self.root.title("P3 File Simple Player")
        self.root.geometry("500x400")

        self.playlist = []
        self.current_index = 0
        self.is_playing = False
        self.is_paused = False
        self.stop_event = threading.Event()
        self.pause_event = threading.Event()
        self.loop_playback = tk.BooleanVar(value=False)  # Loop playback checkbox state

        # Create interface components
        self.create_widgets()

    def create_widgets(self):
        # Playlist
        self.playlist_label = tk.Label(self.root, text="Playlist:")
        self.playlist_label.pack(pady=5)

        self.playlist_frame = tk.Frame(self.root)
        self.playlist_frame.pack(fill=tk.BOTH, expand=True, padx=10, pady=5)

        self.playlist_listbox = tk.Listbox(self.playlist_frame, selectmode=tk.SINGLE)
        self.playlist_listbox.pack(fill=tk.BOTH, expand=True)

        # Checkbox and remove button
        self.checkbox_frame = tk.Frame(self.root)
        self.checkbox_frame.pack(pady=5)

        self.remove_button = tk.Button(self.checkbox_frame, text="Remove File", command=self.remove_files)
        self.remove_button.pack(side=tk.LEFT, padx=5)

        # Loop playback checkbox
        self.loop_checkbox = tk.Checkbutton(self.checkbox_frame, text="Loop Playback", variable=self.loop_playback)
        self.loop_checkbox.pack(side=tk.LEFT, padx=5)

        # Control buttons
        self.control_frame = tk.Frame(self.root)
        self.control_frame.pack(pady=10)

        self.add_button = tk.Button(self.control_frame, text="Add File", command=self.add_file)
        self.add_button.grid(row=0, column=0, padx=5)

        self.play_button = tk.Button(self.control_frame, text="Play", command=self.play)
        self.play_button.grid(row=0, column=1, padx=5)

        self.pause_button = tk.Button(self.control_frame, text="Pause", command=self.pause)
        self.pause_button.grid(row=0, column=2, padx=5)

        self.stop_button = tk.Button(self.control_frame, text="Stop", command=self.stop)
        self.stop_button.grid(row=0, column=3, padx=5)

        # Status label
        self.status_label = tk.Label(self.root, text="Not Playing", fg="blue")
        self.status_label.pack(pady=10)

    def add_file(self):
        files = filedialog.askopenfilenames(filetypes=[("P3 Files", "*.p3")])
        if files:
            self.playlist.extend(files)
            self.update_playlist()

    def update_playlist(self):
        self.playlist_listbox.delete(0, tk.END)
        for file in self.playlist:
            self.playlist_listbox.insert(tk.END, os.path.basename(file))  # Show filename only

    def update_status(self, status_text, color="blue"):
        """Update status label content"""
        self.status_label.config(text=status_text, fg=color)

    def play(self):
        if not self.playlist:
            messagebox.showwarning("Warning", "Playlist is empty!")
            return

        if self.is_paused:
            self.is_paused = False
            self.pause_event.clear()
            self.update_status(f"Now playing: {os.path.basename(self.playlist[self.current_index])}", "green")
            return

        if self.is_playing:
            return

        self.is_playing = True
        self.stop_event.clear()
        self.pause_event.clear()
        self.current_index = self.playlist_listbox.curselection()[0] if self.playlist_listbox.curselection() else 0
        self.play_thread = threading.Thread(target=self.play_audio, daemon=True)
        self.play_thread.start()
        self.update_status(f"Now playing: {os.path.basename(self.playlist[self.current_index])}", "green")

    def play_audio(self):
        while True:
            if self.stop_event.is_set():
                break

            if self.pause_event.is_set():
                time.sleep(0.1)
                continue

            # Check if current index is valid
            if self.current_index >= len(self.playlist):
                if self.loop_playback.get():  # If loop playback is checked
                    self.current_index = 0  # Return to first track
                else:
                    break  # Otherwise stop playback

            file = self.playlist[self.current_index]
            self.playlist_listbox.selection_clear(0, tk.END)
            self.playlist_listbox.selection_set(self.current_index)
            self.playlist_listbox.activate(self.current_index)
            self.update_status(f"Now playing: {os.path.basename(self.playlist[self.current_index])}", "green")
            play_p3_file(file, self.stop_event, self.pause_event)

            if self.stop_event.is_set():
                break

            if not self.loop_playback.get():  # If loop playback is not checked
                break  # Stop after current file

            self.current_index += 1
            if self.current_index >= len(self.playlist):
                if self.loop_playback.get():  # If loop playback is checked
                    self.current_index = 0  # Return to first track

        self.is_playing = False
        self.is_paused = False
        self.update_status("Playback stopped", "red")

    def pause(self):
        if self.is_playing:
            self.is_paused = not self.is_paused
            if self.is_paused:
                self.pause_event.set()
                self.update_status("Playback paused", "orange")
            else:
                self.pause_event.clear()
                self.update_status(f"Now playing: {os.path.basename(self.playlist[self.current_index])}", "green")

    def stop(self):
        if self.is_playing or self.is_paused:
            self.is_playing = False
            self.is_paused = False
            self.stop_event.set()
            self.pause_event.clear()
            self.update_status("Playback stopped", "red")

    def remove_files(self):
        selected_indices = self.playlist_listbox.curselection()
        if not selected_indices:
            messagebox.showwarning("Warning", "Please select files to remove!")
            return

        for index in reversed(selected_indices):
            self.playlist.pop(index)
        self.update_playlist()


if __name__ == "__main__":
    root = tk.Tk()
    app = P3PlayerApp(root)
    root.mainloop()
