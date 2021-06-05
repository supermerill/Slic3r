import re
from datetime import date

datastore = dict();
datastore_trim = dict();# key:string -> TranslationLine

regex_only_letters = re.compile(r"[^a-zA-Z]")
allow_msgctxt = True;
ignore_case = False;
remove_comment = False;

def trim(str):
	redo = True;
	while redo:
		while len(str) > 0 and (
					str[0] == ":"
				or str[0] == "."
				or str[0] == ","
				or str[0] == "!"
				):
			str = str[1:];
		while len(str) > 0 and (
					str[-1] == ":"
				or str[-1] == "."
				or str[-1] == ","
				or str[-1] == "!"
				):
			str = str[:-1];
		str_stripped = str.strip();
		if str == str_stripped:
			redo = False
		else:
			str = str_stripped;
	return str;

class TranslationFiles:
	file_in = ""
	file_out = ""
	file_todo = ""

class TranslationLine:
	header_comment = ""
	raw_msgid = ""
	msgid = ""
	raw_msgstr = ""
	msgstr = ""
	multivalue = False

def main():
	global datastore, datastore_trim, regex_only_letters, allow_msgctxt, ignore_case, remove_comment;
	data_files = list(); # list of file paths
	ui_dir = "";
	operations = list(); # list of TranslationFiles
	settings_stream = open("./settings.ini", mode="r", encoding="utf-8")
	lines = settings_stream.read().splitlines()
	for line in lines:
		if line.startswith("data"):
			data_files.append(line[line.index('=')+1:].strip());
		
		if line.startswith("input"):
			operations.append(TranslationFiles());
			operations[-1].file_in = line[line.index('=')+1:].strip();
		
		if line.startswith("output") and operations:
			operations[-1].file_out = line[line.index('=')+1:].strip();
		
		if line.startswith("todo") and operations:
			operations[-1].file_todo = line[line.index('=')+1:].strip();
		
		if line.startswith("ui_dir"):
			ui_dir = line[line.index('=')+1:].strip();
		
		if line.startswith("allow_msgctxt"):
			allow_msgctxt = (line[line.index('=')+1:].strip().lower() == "true");
			print("Don't comment msgctxt" if allow_msgctxt else "Commenting msgctxt");
		
		if line.startswith("allow_msgctxt"):
			allow_msgctxt = (line[line.index('=')+1:].strip().lower() == "true");
			print("Don't comment msgctxt" if allow_msgctxt else "Commenting msgctxt");

		if line.startswith("remove_comment"):
			remove_comment = (line[line.index('=')+1:].strip().lower() == "true");
			if remove_comment:
				print("Will not output the comments");


	# all_lines = list();
	for data_file in data_files:
		new_data = createKnowledge(data_file);
		for dataline in new_data:
			if not dataline.msgid in datastore:
				datastore[dataline.msgid] = dataline;
				datastore_trim[trim(dataline.msgid)] = dataline;
				if dataline.msgid == " Layers,":
					print(trim(dataline.msgid)+" is inside? "+("oui" if "Layers" in datastore_trim else "non"));
			else:
				str_old_val = datastore[dataline.msgid].msgstr;
				str_test_val = dataline.msgstr;
				length_old = len(regex_only_letters.sub("", str_old_val));
				length_new = len(regex_only_letters.sub("", str_test_val));
				# if already exist, only change it if the previous was lower than 3 char
				if length_new > length_old and length_old < 3:
					print(str_old_val.replace('\n', ' ')+" replaced by "+str_test_val.replace('\n', ' '));
					datastore[dataline.msgid].msgstr = str_test_val;
					datastore_trim[trim(dataline.msgid)].msgstr = str_test_val;
		print("finish reading" + data_file + " of size "+ str(len(new_data)) + ", now we had "+ str(len(datastore)) + " items");

	if ignore_case:
		temp = list();
		for msgid in datastore:
			if not msgid.lower() in datastore:
				temp.append(msgid);
		for msgid in temp:
			datastore[msgid.lower()] = datastore[msgid];
		temp = list();
		for msgid in datastore_trim:
			if not msgid.lower() in datastore_trim:
				temp.append(msgid);
		for msgid in temp:
			datastore_trim[msgid.lower()] = datastore_trim[msgid];

	for operation in operations:
		print("Translating " + operation.file_in);
		dict_ope = dict();
		ope_file_in = list();
		lst_temp = createKnowledge(operation.file_in);
		print("String from source files: " + str(len(lst_temp)));
		nbTrans = 0;
		#remove duplicate
		for line in lst_temp:
			if not line.msgid in dict_ope:
				dict_ope[line.msgid] = line;
				ope_file_in.append(line);
				if line.msgstr:
					nbTrans+=1;
					print(line.header_comment);
					print(line.raw_msgid);
					print(line.msgid);
					print(line.raw_msgstr);
					print(line.msgstr);
		#add def from conf files
		if ui_dir:
			new_data = parse_ui_file(ui_dir+"/extruder.ui");
			new_data.extend(parse_ui_file(ui_dir+"/extruder.ui"));
			new_data.extend(parse_ui_file(ui_dir+"/filament.ui"));
			new_data.extend(parse_ui_file(ui_dir+"/milling.ui"));
			new_data.extend(parse_ui_file(ui_dir+"/print.ui"));
			new_data.extend(parse_ui_file(ui_dir+"/printer_fff.ui"));
			new_data.extend(parse_ui_file(ui_dir+"/printer_sla.ui"));
			new_data.extend(parse_ui_file(ui_dir+"/sla_material.ui"));
			new_data.extend(parse_ui_file(ui_dir+"/sla_print.ui"));
			print("String from ui files: " + str(len(new_data)));
			for dataline in new_data:
				if not dataline.msgid in dict_ope:
					dict_ope[dataline.msgid] = dataline;
					ope_file_in.append(dataline);
		print("String to translate: " + str(len(ope_file_in) - nbTrans)+" and already translated: "+str(nbTrans));
		
		#create TODO file
		if operation.file_todo:
			outputUntranslated(ope_file_in, operation.file_todo);
		
		#create .po file
		if operation.file_out:
			translate(ope_file_in, operation.file_out);
	
	print("End of merge");

def createKnowledge(file_path_in):
	read_data_lines = list();
	try:
		file_in_stream = open(file_path_in, mode="r", encoding="utf-8")
		lines = file_in_stream.read().splitlines();
		lines.append("");
		line_idx = 0;
		current_line = TranslationLine();
		nb = 0;
		while line_idx < len(lines):
			if not lines[line_idx].startswith("msgid") or len(lines[line_idx]) <= 7:
				if (lines[line_idx].startswith("#") 
					or lines[line_idx].startswith("msgctxt")
					or len(lines[line_idx].strip()) == 0
					):
					if not allow_msgctxt and lines[line_idx].startswith("msgctxt"):
						current_line.header_comment += "\n#, " + lines[line_idx];
					else:
						current_line.header_comment += "\n" + lines[line_idx];
				line_idx+=1;
				continue;

			# get the msgid line
			current_line.raw_msgid = lines[line_idx];
			current_line.msgid = lines[line_idx][7:];

			#get the next line (can be whatever)
			line_idx+=1;
			if line_idx >= len(lines):
				return read_data_lines;

			#populate the full current_line.msgid string
			while lines[line_idx].startswith("\"") or lines[line_idx].startswith("msgid"):
				current_line.raw_msgid += "\n" + lines[line_idx];
				if lines[line_idx].startswith("msgid"):
					current_line.multivalue = True;
				if lines[line_idx].startswith("\""):
					current_line.msgid = current_line.msgid[0:-1];
					current_line.msgid += lines[line_idx][1:];
				else:
					current_line.msgid += "\n" + lines[line_idx];
				#todo: do something for msgid_plural. Not needed right now...
				
				#get the next line (can be whatever)
				line_idx+=1;
				if line_idx >= len(lines):
					return read_data_lines;

			#check validity of the id
			if len(current_line.msgid) < 3:
				current_line = TranslationLine();
				continue;
			current_line.msgid = current_line.msgid[0:-1];

			#there should be a msgstr just after
			if not lines[line_idx].startswith("msgstr") or len(lines[line_idx]) <= 8:
				current_line = TranslationLine();
				continue;

			current_line.raw_msgstr = lines[line_idx];
			if lines[line_idx][7] == "\"":
				current_line.msgstr = lines[line_idx][8:];
			elif lines[line_idx][6] == "[":
				current_line.msgstr = lines[line_idx][11:];
			else:
				#can't parse
				print("error, can't parse msgstr: '"+lines[line_idx]+"'");
				current_line.msgstr = "";

			line_idx+=1;
			if line_idx >= len(lines):
				return read_data_lines;

			while lines[line_idx].startswith("\"") or lines[line_idx].startswith("msgstr"):
				current_line.raw_msgstr += "\n" + lines[line_idx];
				if lines[line_idx].startswith("\""):
					current_line.msgstr = current_line.msgstr[0:-1];
					current_line.msgstr += lines[line_idx][1:];
				elif lines[line_idx].startswith("msgstr["):
					current_line.msgstr = lines[line_idx][11:];
					current_line.multivalue = True;
				else:
					current_line.msgstr += "\n" + lines[line_idx];

				#get the next line (can be whatever)
				line_idx+=1;
				if line_idx >= len(lines):
					return read_data_lines;
			
			if current_line.msgstr:
				current_line.msgstr = current_line.msgstr[0:-1];
			read_data_lines.append(current_line);
			current_line = TranslationLine();
	except Exception as error:
		print("error, cannot read file " + file_path_in);
		print(error);
	return read_data_lines;

def getTranslation(item):
	if len(item.msgid) == 0:
		return "";
	if item.msgid in datastore:
		return datastore[item.msgid].raw_msgstr;
	elif item.msgid in datastore_trim:
		good = datastore_trim[item.msgid];
		if not good.multivalue:
			return "msgstr \""+trim(good.msgstr)+"\"";
	else:
		item_msg_trim = trim(item.msgid);
		if item_msg_trim in datastore:
			good = datastore[trim(item.msgid)];
			if not good.multivalue:
				if good.msgid in item.msgid:
					start_at = item.msgid.index(good.msgid);
					return "msgstr \"" + item.msgid[0:start_at] + good.msgstr + item.msgid[start_at+len(good.msgid):] + "\"";
		elif item_msg_trim in datastore_trim:
			good = datastore_trim[item_msg_trim];
			if not good.multivalue:
				good_msg_trim = trim(good.msgid);
				if good_msg_trim in item.msgid:
					start_at = item.msgid.index(good_msg_trim);
					return "msgstr \"" + item.msgid[0:start_at] + trim(good.msgstr) + item.msgid[start_at+len(good_msg_trim):] + "\"";

	if ignore_case:
		lowercase = TranslationLine();
		lowercase.msgid = item.msgid.lower();
		if lowercase.msgid != item.msgid:
			lowercase.header_comment = item.header_comment;
			lowercase.raw_msgid = item.raw_msgid;
			lowercase.raw_msgstr = item.raw_msgstr;
			lowercase.msgstr = item.msgstr;
			lowercase.multivalue = item.multivalue;
			return getTranslation(lowercase)
	return "";


def outputUntranslated(data_to_translate, file_path_out):
	try:
		file_out_stream = open(file_path_out, mode="w", encoding="utf-8")
		nb_lines = 0;
		#sort to have an easier time trnaslating.
		# idealy, they shoud be grouped by proximity, but it's abit more complicated to code
		sorted_lines = list()
		for dataline in data_to_translate:
			if not dataline.msgstr.strip() and dataline.msgid and len(getTranslation(dataline).strip()) == 0:
				sorted_lines.append(dataline);
		sorted_lines.sort(key=lambda x:x.msgid.lower())

		# output bits that are empty
		for dataline in sorted_lines:
			file_out_stream.write(dataline.header_comment)
			file_out_stream.write("\n")
			file_out_stream.write(dataline.raw_msgid)
			file_out_stream.write("\n")
			file_out_stream.write(dataline.raw_msgstr)
			file_out_stream.write("\n")
			nb_lines+=1;

		print("There is " + str(nb_lines) +" string untranslated");
	except Exception as error:
		print("error, cannot write file " + file_path_out);
		print(error);

def translate(data_to_translate, file_path_out):
	# try:
	file_out_stream = open(file_path_out, mode="w", encoding="utf-8")
	file_out_stream.write("# Translation file for ???\n");
	file_out_stream.write("# Copyright (C) 2021\n");
	file_out_stream.write("# This file is distributed under the same license as Slic3r.\n");
	file_out_stream.write("#\n");
	file_out_stream.write("msgid \"\"\n");
	file_out_stream.write("msgstr \"\"\n");
	file_out_stream.write("\"Project-Id-Version: Slic3r\\n\"\n");
	file_out_stream.write("\"POT-Creation-Date: "+date.today().strftime('%Y-%m-%d %H:%M%z')+"\\n\"\n");
	file_out_stream.write("\"PO-Revision-Date: "+date.today().strftime('%Y-%m-%d %H:%M%z')+"\\n\"\n");
	file_out_stream.write("\"Last-Translator:\\n\"\n");
	file_out_stream.write("\"Language-Team:\\n\"\n");
	file_out_stream.write("\"MIME-Version: 1.0\\n\"\n");
	file_out_stream.write("\"Content-Type: text/plain; charset=UTF-8\\n\"\n");
	file_out_stream.write("\"Content-Transfer-Encoding: 8bit\\n\"\n");
	file_out_stream.write("\"Language:\\n\"\n");
	nb_lines = 0;
	data_to_translate.sort(key=lambda x:x.msgid.lower().strip())
	# translate bits that are empty
	for dataline in data_to_translate:
		if not dataline.msgstr.strip():
			transl = getTranslation(dataline)
			if len(transl) > 9 or ( len(transl) > 3 and not transl.startswith('msgstr "')):
				file_out_stream.write("\n")
				if not remove_comment:
					file_out_stream.write(dataline.header_comment.strip())
					file_out_stream.write("\n")
				file_out_stream.write(dataline.raw_msgid)
				file_out_stream.write("\n")
				file_out_stream.write(transl)
				file_out_stream.write("\n")
				nb_lines+=1;
				if dataline.raw_msgid.count('%') != transl.count('%'):
					print("WARNING: not same number of '%' ( "+ str(dataline.raw_msgid.count('%')) + " => " + str(transl.count('%')) + ")"
						+"\n  for string:'" + dataline.msgid + "  '\n=>'"+transl[8:]);
		else:
			file_out_stream.write("\n")
			if not remove_comment:
				file_out_stream.write(dataline.header_comment.strip())
				file_out_stream.write("\n")
			file_out_stream.write(dataline.raw_msgid)
			file_out_stream.write("\n")
			file_out_stream.write(dataline.raw_msgstr)
			file_out_stream.write("\n")
			if dataline.raw_msgid.count('%') != dataline.raw_msgstr.count('%'):
				print("WARNING: not same number of '%'( "+ str(dataline.raw_msgid.count('%')) + " => " + str(dataline.raw_msgstr.count('%')) + ")"
						+"\n  for string:'" + dataline.msgid + "  '\n=>'"+dataline.msgstr);
			nb_lines+=1;
	print("There is " + str(nb_lines) +" string translated in the .po");
	# except Exception as error:
		# print("error, cannot write file " + file_path_out);
		# print(error);

def parse_ui_file(file_path):
	read_data_lines = list();
	# try:
	file_in_stream = open(file_path, mode="r", encoding="utf-8")
	lines = file_in_stream.read().splitlines();
	lines.append("");
	line_idx = 0;
	nb = 0;
	while line_idx < len(lines):
		items = lines[line_idx].strip().split(":");
		if len(items) > 1:
			if items[0]=="page:
				current_line = TranslationLine();
				current_line.header_comment = "\n#: "+file_path;#+":"+str(line_idx);
				current_line.raw_msgid = "msgid \""+items[1]+"\"";
				current_line.msgid = items[1];
				current_line.raw_msgstr = "msgstr \"\"";
				current_line.msgstr = "";
				read_data_lines.append(current_line);
			if items[0]=="group" or items[0]=="line":
				current_line = TranslationLine();
				current_line.header_comment = "\n#: "+file_path;#+":"+str(line_idx);
				current_line.raw_msgid = "msgid \""+items[-1]+"\"";
				current_line.msgid = items[-1];
				current_line.raw_msgstr = "msgstr \"\"";
				current_line.msgstr = "";
				read_data_lines.append(current_line);
			if items[0]=="setting":
				for item in items:
					if item.startswith("label$") or item.startswith("sidetext$") or item.startswith("sidetext$"):
						current_line = TranslationLine();
						current_line.header_comment = "\n#: "+file_path+" : l"+str(line_idx);
						current_line.msgid = item.split("$")[-1];
						current_line.raw_msgid = "msgid \""+current_line.msgid+"\"";
						current_line.raw_msgstr = "msgstr \"\"";
						current_line.msgstr = "";
						read_data_lines.append(current_line);
		line_idx+=1;
	
	return read_data_lines;


	








main();
