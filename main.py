import os
import sys
import subprocess
import argparse
import datetime
import traceback
import openai
import shutil
import glob
openai.api_key = os.getenv('OPENAI_API_KEY')

script_folder_path = os.path.dirname(os.path.realpath(__file__))
build_folder_path = os.path.join(script_folder_path, "build")


def git_push(build_successful):
    os.chdir(script_folder_path)
    if build_successful:
        try:
            files_to_commit = ['main.py', 'server.cpp', 'client.cpp', 'CMakeLists.txt', 'prompt.txt']

            for file_name in files_to_commit:
                subprocess.run(['git', 'add', file_name], check=True)

            commit_message = f"Update {', '.join(files_to_commit)} (automated commit)"
            subprocess.run(['git', 'commit', '-m', commit_message], check=True)
            subprocess.run(['git', 'push'], check=True)

        except Exception as e:
            print(f"Error while committing and pushing to Git: {e}")
            formatted_traceback = traceback.format_exc()
            print(f"Traceback:\n{formatted_traceback}")

def archive_responses():
    response_files = sorted(glob.glob("response_*.md"), reverse=True)
    if not os.path.exists('archives'):
        os.mkdir('archives')

    for response_file in response_files[1:]:
        shutil.move(response_file, f"archives/{response_file}")


def get_file_content(file_path: str):
    with open(file_path, 'r') as f:
        return f.read()

def write_source_code():
    current_datetime = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    uname_output = subprocess.run(['uname', '-a'], capture_output=True, text=True).stdout.strip()

    state_content = f"{current_datetime}\n{uname_output}\n"
    os.chdir(script_folder_path)
    print(os.getcwd())

    for file_name in os.listdir('.'):
        if file_name.endswith('.py') or file_name.endswith('CMakeLists.txt') or file_name.endswith('cpp'):
            state_content += f"--- {file_name} ---\n"
            state_content += get_file_content(file_name)
            state_content += "\n\n"

    with open('state.txt', 'w') as f:
        f.write(state_content)


def write_state_file(server_output_msg: str, client_output_msg: str, command: str):
    write_source_code()
    state_content = ""
    state_content += f"Command used: {command}\n\n"
    state_content += f"build datetime: {datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S.%f')}\n\n"
    state_content += f"Server output messages:\n{server_output_msg}\n\n"
    state_content += f"Client output messages:\n{client_output_msg}\n\n"

    os.chdir(script_folder_path)
    with open(f"state.txt", "a") as f:
        f.write(state_content)

def build() -> bool:
    try:
        if not os.path.exists('build'):
            command = ["mkdir", "build"]
            subprocess.run(command, check=True)

        os.chdir("build")

        command = ["cmake", "..", "-DCMAKE_BUILD_TYPE=Debug"]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

        command = ["cmake", "--build", "."]  # Corrected the build command
        result2 = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

        server_output_msg = f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        client_output_msg = f"stdout:\n{result2.stdout}\nstderr:\n{result2.stderr}"

        build_successful = result.returncode == 0 and result2.returncode == 0

        if build_successful:
            archive_responses()
            
        write_state_file(server_output_msg, client_output_msg, command)

        return build_successful

    except Exception as e:
        print(f"Error: {e}")
        formatted_traceback = traceback.format_exc()
        write_state_file(f"Traceback:\n{formatted_traceback}", "", command)
        return False

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f"Usage: python main.py <server-binary> <client-binary> [--send] [--push]")
        sys.exit(1)

    parser = argparse.ArgumentParser()
    parser.add_argument("server", help="Server binary target", type=str)
    parser.add_argument("client", help="Client binary target", type=str)
    parser.add_argument("--send", help="Send the response to GPT", action="store_true")
    parser.add_argument("--push", help="Push the code to GitHub on a new branch", action="store_true")
    args = parser.parse_args()

    build_success = build()

    if args.push and build_success:
        git_push(True)

    if args.send:
        os.chdir(script_folder_path)
        prompt = open('prompt.txt').read()
        state = open('state.txt').read()
        messages = [
                {
                    'role': 'system',
                    'content': state
                },
                {
                    'role': 'user',
                    'content': prompt
                }
            ]

        print(f'sending with prompt: {prompt}...')
        response = openai.ChatCompletion.create(
            model='gpt-4',
            messages=messages,
            stream=True
        )

        answer = ''
        timestamp = datetime.datetime.now().strftime('%H_%M_%S')
        response_filename = f"response_{timestamp}.md"

        with open(response_filename, "w") as response_file:
            for chunk in response:
                try:
                    s = chunk['choices'][0]['delta']['content']

                    answer += s
                    print(s, end='')
                    response_file.write(s)
                    response_file.flush()  # ensure content is written to file immediately
                except:
                    pass
