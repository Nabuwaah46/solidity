/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
/**
 * Unit tests for the address checksum.
 */

#include <lsp/VFS.h>
#include <libsolutil/Exceptions.h>

#include <test/Common.h>

#include <boost/test/unit_test.hpp>

namespace std {
	ostream& operator<<(ostream& _os, lsp::vfs::TextLines const& _lines)
	{
		_os << '{';
		for (size_t i = 0; i < _lines.size(); ++i)
			_os << (i ? ", " : "") << '"' << _lines[i] << '"';
		_os << '}';
		return _os;
	}
}

using namespace std;

namespace lsp::test
{

BOOST_AUTO_TEST_SUITE(LSP)

BOOST_AUTO_TEST_CASE(VFS_create)
{
	vfs::VFS vfs;
	vfs.insert("file:///project/test.txt", "text", 1, "Hello, World\n");

	auto const file = vfs.find("file:///project/test.txt");
	BOOST_CHECK_NE(file, nullptr);

	BOOST_CHECK_EQUAL(file->uri(), "file:///project/test.txt");
	BOOST_CHECK_EQUAL(file->languageId(), "text");
	BOOST_CHECK_EQUAL(file->version(), 1);

	BOOST_CHECK_EQUAL(file->str(), "Hello, World\n\n");
}

BOOST_AUTO_TEST_CASE(VFS_modify_erase)
{
	vfs::VFS vfs;
	vfs::File& file = vfs.insert("file:///project/test.txt", "text", 1, "Hello, World\n");
	file.modify(Range{{0, 0}, {0, 1}}, "");

	BOOST_CHECK_EQUAL(file.str(), "ello, World\n\n");

	file.modify(Range{{0, 4}, {0, 5}}, "");
	BOOST_CHECK_EQUAL(file.str(), "ello World\n\n");

	file.modify(Range{{0, 4}, {0, 10}}, "");
	BOOST_CHECK_EQUAL(file.str(), "ello\n\n");
}

BOOST_AUTO_TEST_CASE(VFS_modify_erase_multiline)
{
	vfs::VFS vfs;
	vfs::File& file = vfs.insert("file:///project/test.txt", "text", 1, "Hello,\nWorld\nCrew\n");
	file.modify(Range{{0, 1}, {2, 2}}, "");

	BOOST_CHECK_EQUAL(file.str(), "Hew\n\n");
}

BOOST_AUTO_TEST_CASE(VFS_modify_change)
{
	vfs::VFS vfs;
	vfs::File& file = vfs.insert("file:///project/test.txt", "text", 1, "Hello, World\n");
	file.modify(Range{{0, 5}, {0, 6}}, ";");

	BOOST_CHECK_EQUAL(file.str(), "Hello; World\n\n");
}

BOOST_AUTO_TEST_CASE(VFS_modify_change_single_to_multi_line2)
{
	// replace fragment of a single line with 2 lines
	vfs::VFS vfs;
	vfs::File& file = vfs.insert("file:///project/test.txt", "text", 1, "Hello\nWorld\n");
	file.modify(Range{{0, 1}, {0, 2}}, "{foo\nbar}");

	BOOST_CHECK_EQUAL(file.str(), "H{foo\nbar}llo\nWorld\n\n");
}

BOOST_AUTO_TEST_CASE(VFS_modify_change_single_to_multi_line3)
{
	// replace fragment of a single line with 3 lines
	vfs::VFS vfs;
	vfs::File& file = vfs.insert("file:///project/test.txt", "text", 1, "Hello\nWorld\n");
	file.modify(Range{{0, 1}, {0, 2}}, "{foo\nbar\ncom}");

	BOOST_CHECK_EQUAL(file.str(), "H{foo\nbar\ncom}llo\nWorld\n\n");
}

BOOST_AUTO_TEST_CASE(VFS_modify_change_single_to_multi_line3_last_empty)
{
	// replace fragment of a single line with 3 lines
	vfs::VFS vfs;
	vfs::File& file = vfs.insert("file:///project/test.txt", "text", 1, "Hello\nWorld\n");
	file.modify(Range{{0, 1}, {0, 2}}, "{foo\nbar}\n");

	BOOST_CHECK_EQUAL(file.str(), "H{foo\nbar}\nllo\nWorld\n\n");
}

BOOST_AUTO_TEST_CASE(VFS_modify_insert_at_the_beginning)
{
	vfs::VFS vfs;
	vfs::File& file = vfs.insert("file:///project/test.txt", "text", 1, "Hello, World\n");

	file.modify(Range{{0, 0}, {0, 0}}, "[");
	BOOST_CHECK_EQUAL(file.str(), "[Hello, World\n\n");

	vfs::File& file2 = vfs.insert("file:///project/test.txt", "text", 1, "Hello,\nWorld\n");
	file2.modify(Range{{1, 0}, {1, 0}}, "[");
	BOOST_CHECK_EQUAL(file2.str(), "Hello,\n[World\n\n");

	file2.modify(Range{{2, 0}, {2, 0}}, "[");
	BOOST_CHECK_EQUAL(file2.str(), "Hello,\n[World\n[\n");
}

BOOST_AUTO_TEST_CASE(VFS_modify_insert)
{
	vfs::VFS vfs;
	vfs::File& file = vfs.insert("file:///project/test.txt", "text", 1, "Hello, World\n");
	file.modify(Range{{0, 5}, {0, 5}}, ";");

	BOOST_CHECK_EQUAL(file.str(), "Hello;, World\n\n");
}

BOOST_AUTO_TEST_SUITE_END()

} // end namespace
